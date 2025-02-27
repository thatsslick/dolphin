// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Movie.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <iterator>
#include <mbedtls/config.h>
#include <mbedtls/md.h>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Hash.h"
#include "Common/IOFile.h"
#include "Common/MsgHandler.h"
#include "Common/NandPaths.h"
#include "Common/StringUtil.h"
#include "Common/Timer.h"
#include "Common/Version.h"

#include "Core/Boot/Boot.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/ConfigLoaders/MovieConfigLoader.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/LUA/Lua.h"
#include "Core/DSP/DSPCore.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/EXI/EXI_DeviceIPL.h"
#include "Core/HW/EXI/EXI_DeviceMemoryCard.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteCommon/DataReport.h"
#include "Core/HW/WiimoteCommon/WiimoteReport.h"

#include "Core/HW/WiimoteEmu/Encryption.h"
#include "Core/HW/WiimoteEmu/Extension/Classic.h"
#include "Core/HW/WiimoteEmu/Extension/Nunchuk.h"
#include "Core/HW/WiimoteEmu/ExtensionPort.h"
#include "Core/PowerPC/MMU.h"

#include "Core/IOS/USB/Bluetooth/BTEmu.h"
#include "Core/IOS/USB/Bluetooth/WiimoteDevice.h"
#include "Core/NetPlayProto.h"
#include "Core/State.h"
#include "Core/WiiUtils.h"

#include "DiscIO/Enums.h"

#include "InputCommon/GCPadStatus.h"

#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoConfig.h"

// The chunk to allocate movie data in multiples of.
#define DTM_BASE_LENGTH (1024)

namespace Movie
{
using namespace WiimoteCommon;
using namespace WiimoteEmu;

static bool s_bReadOnly = true;
static u32 s_rerecords = 0;
static PlayMode s_playMode = MODE_NONE;

static std::array<ControllerType, 4> s_controllers{};
static std::array<bool, 4> s_wiimotes{};
static ControllerState s_padState;
static DTMHeader tmpHeader;
static std::vector<u8> s_temp_input;
static u64 s_currentByte = 0;
static u64 s_currentFrame = 0, s_totalFrames = 0;  // VI
static u64 s_currentLagCount = 0;
static u64 s_totalLagCount = 0;                               // just stats
static u64 s_currentInputCount = 0, s_totalInputCount = 0;    // just stats
static u64 s_totalTickCount = 0, s_tickCountAtLastInput = 0;  // just stats
static u64 s_recordingStartTime;  // seconds since 1970 that recording started
static bool s_bSaveConfig = false, s_bNetPlay = false;
static bool s_bClearSave = false;
static bool s_bDiscChange = false;
static bool s_bReset = false;
static std::string s_author;
static std::string s_discChange;
static std::array<u8, 16> s_MD5;
static u8 s_bongos, s_memcards;
static std::array<u8, 20> s_revision;
static u32 s_DSPiromHash = 0;
static u32 s_DSPcoefHash = 0;

static bool s_bRecordingFromSaveState = false;
static bool s_bPolled = false;

// s_InputDisplay is used by both CPU and GPU (is mutable).
static std::mutex s_input_display_lock;
static std::string s_InputDisplay[8];

static GCManipFunction s_gc_manip_func;
static WiiManipFunction s_wii_manip_func;

static std::string s_current_file_name;

static void GetSettings();
static bool IsMovieHeader(const std::array<u8, 4>& magic)
{
  return magic[0] == 'D' && magic[1] == 'T' && magic[2] == 'M' && magic[3] == 0x1A;
}

static std::array<u8, 20> ConvertGitRevisionToBytes(const std::string& revision)
{
  std::array<u8, 20> revision_bytes{};

  if (revision.size() % 2 == 0 && std::all_of(revision.begin(), revision.end(), ::isxdigit))
  {
    // The revision string normally contains a git commit hash,
    // which is 40 hexadecimal digits long. In DTM files, each pair of
    // hexadecimal digits is stored as one byte, for a total of 20 bytes.
    size_t bytes_to_write = std::min(revision.size() / 2, revision_bytes.size());
    unsigned int temp;
    for (size_t i = 0; i < bytes_to_write; ++i)
    {
      sscanf(&revision[2 * i], "%02x", &temp);
      revision_bytes[i] = temp;
    }
  }
  else
  {
    // If the revision string for some reason doesn't only contain hexadecimal digit
    // pairs, we instead copy the string with no conversion. This probably doesn't match
    // the intended design of the DTM format, but it's the most sensible fallback.
    size_t bytes_to_write = std::min(revision.size(), revision_bytes.size());
    std::copy_n(std::begin(revision), bytes_to_write, std::begin(revision_bytes));
  }

  return revision_bytes;
}

// NOTE: GPU Thread
std::string GetInputDisplay()
{
  if (!IsMovieActive())
  {
    s_controllers = {};
    s_wiimotes = {};
    for (int i = 0; i < 4; ++i)
    {
      if (SerialInterface::GetDeviceType(i) == SerialInterface::SIDEVICE_GC_GBA_EMULATED)
        s_controllers[i] = ControllerType::GBA;
      else if (SerialInterface::GetDeviceType(i) != SerialInterface::SIDEVICE_NONE)
        s_controllers[i] = ControllerType::GC;
      else
        s_controllers[i] = ControllerType::None;
      s_wiimotes[i] = WiimoteCommon::GetSource(i) != WiimoteSource::None;
    }
  }

  std::string input_display;
  {
    std::lock_guard guard(s_input_display_lock);
    for (int i = 0; i < 4; ++i)
    {
      if (IsUsingPad(i))
        input_display += s_InputDisplay[i] + '\n';
    }
    for (int i = 0; i < 4; ++i)
    {
      if (IsUsingWiimote(i))
        input_display += s_InputDisplay[i + 4] + '\n';
    }
  }
  return input_display;
}

	std::string GetRAMDisplay()
	{
	    std::string RAMDisplay = "";

		// Dragonbane
	    std::string gameID = SConfig::GetInstance().GetGameID();
	    std::string iniContent;

	    bool success =
	        File::ReadFileToString(File::GetSysDirectory() +  "/InfoDisplay/" + gameID + ".ini", iniContent);

	    if (success)
	    {
		    int lineCounter = 0;
		    bool inProgress = true;
		    RAMDisplay.append("\n");

		    while (inProgress)
		    {
			    lineCounter++;

			    std::string lineName = StringFromFormat("Line%i", lineCounter);

			    std::string::size_type loc = iniContent.find(lineName, 0);
			    if (loc == std::string::npos)
			    {
				    inProgress = false;
				    break;
			    }

			    iniContent = iniContent.substr(loc);
			    iniContent = iniContent.substr(iniContent.find("\"", 0) + 1);

			    std::string line = iniContent.substr(0, iniContent.find("\"", 0));
			    std::string blockContent = iniContent.substr(0, iniContent.find("End Line", 0));

			    std::string::size_type locNext = line.find("%", 0);
			    std::string subLine = line;
			    int argCounter = 0;

			    while (locNext != std::string::npos)
			    {
				    argCounter++;

				    std::string currSectionOutput = subLine.substr(0, locNext);
				    subLine = subLine.substr(locNext + 1);
				    std::string currIdenti = subLine.substr(0, 3);

				    int numBytes = atoi(currIdenti.substr(1, 1).c_str());
				    std::string identifier = "%" + currIdenti.substr(0, 1);
				    u32 readAddress;

				    subLine = subLine.substr(3);

				    std::string nextArgName = StringFromFormat("Arg%i", argCounter);

				    std::string::size_type locNextArg = blockContent.find(nextArgName, 0);

				    if (locNextArg == std::string::npos)
					    break;

				    std::string argString = blockContent.substr(locNextArg);
				    argString = argString.substr(argString.find("=", 0) + 1);
				    argString = argString.substr(0, argString.find(";", 0));

				    std::string::size_type locPlus = argString.find("+", 0);
				    std::string::size_type locHint = argString.find(">>", 0);

				    std::string currHint;

				    if (locHint != std::string::npos)
					    currHint = argString.substr(locHint + 3);

				    if (locPlus == std::string::npos)
				    {
					    std::string arguString;

					    if (locHint != std::string::npos)
					    {
						    arguString = argString.substr(0, locHint - 1);
					    }
					    else
					    {
						    arguString = argString;
					    }

					    readAddress = strtol(arguString.c_str(), nullptr, 16);
				    }
				    else
				    {
					    u32 pointerAddress;
					    u32 offset = 0;

					    pointerAddress = strtol(argString.substr(0, locPlus - 1).c_str(), nullptr, 16);

						std::string arguString = argString.substr(locPlus + 2);

						if (locHint != std::string::npos)
						{
							locHint = arguString.find(">>", 0);

							arguString = arguString.substr(0, locHint - 1);
						}
						
						while (locPlus != std::string::npos)
						{
							offset = strtol(arguString.c_str(), nullptr, 16);
							pointerAddress = Lua::readPointer(pointerAddress, offset);
							locPlus = argString.find("+", locPlus + 1);
							arguString = argString.substr(locPlus + 2);
						}			    
						
						readAddress = pointerAddress;

						if ((readAddress == 0) || (readAddress == offset))
						{
							RAMDisplay.append(currSectionOutput + "N/A");
							locNext = subLine.find("%", 0);
							continue;
						}
				    }
									   					 				  


				    std::string finalOutput;

				    if (identifier.compare("%s") == 0)
				    {
                                            std::string outputString = PowerPC::Read_String(readAddress, numBytes);
                                            /*
					    std::string outputString = "";
                                            
                                            PowerPC::TryReadResult<std::string> result = HostTryReadString(readAddress + 0x80000000, numBytes, PowerPC::RequestedAddressSpace::Virtual);
                                            if (result)
                                                outputString = result.value;
                                                */

					    finalOutput = StringFromFormat(identifier.c_str(), outputString.c_str());
				    }
				    else if (identifier.compare("%f") == 0)
				    {
					    float outputFloat = 0;

                                            PowerPC::TryReadResult<float> result = HostTryReadF32(readAddress + 0x80000000, PowerPC::RequestedAddressSpace::Virtual);
                                            if (result)
                                                outputFloat = result.value;

					    finalOutput = StringFromFormat(identifier.c_str(), outputFloat);
				    }
				    else if (numBytes == 4)
				    {
					    u32 output4Bytes = 0;

                                            PowerPC::TryReadResult<u32> result = HostTryReadU32(readAddress + 0x80000000, PowerPC::RequestedAddressSpace::Virtual);
                                            if (result)
                                                output4Bytes = result.value;

					    finalOutput = StringFromFormat(identifier.c_str(), output4Bytes);
				    }
				    else if (numBytes == 2)
				    {
					    u16 output2Bytes = 0;

                                            PowerPC::TryReadResult<u16> result = HostTryReadU16(readAddress + 0x80000000, PowerPC::RequestedAddressSpace::Virtual);
                                            if (result)
                                                output2Bytes = result.value;

					    // Special Formatting for 2 Byte
					    if (currHint.compare("Degrees") == 0)
					    {
						    double degrees = output2Bytes;
						    degrees = (degrees / 182.04) + 0.5;

						    int finalDegrees = (int)degrees;

						    if (finalDegrees >= 360)
							    finalDegrees = finalDegrees - 360;

						    std::string newIdentifier = identifier;
						    newIdentifier.append(" (%i DEG)");

						    finalOutput = StringFromFormat(newIdentifier.c_str(), output2Bytes, finalDegrees);
					    }
					    else if (currHint.compare("seconds") == 0)
					    {
						    // ToD
						    int time = output2Bytes;
						    int seconds = (time / 60) % 60;

						    float miliseconds = (float)time / 60;
						    miliseconds = (miliseconds - seconds) * 999;

						    int finalMinutes = (int)miliseconds;

						    std::stringstream ss;

						    ss << finalMinutes;
						    std::string minutesString = ss.str();

						    if (finalMinutes < 10)
							    minutesString = "0" + minutesString;

						    std::string newIdentifier = identifier;
						    newIdentifier.append(".%s");

						    finalOutput = StringFromFormat(newIdentifier.c_str(), seconds, minutesString.c_str());
					    }
					    else if (currHint.compare("minutes") == 0)
					    {
						    // ToD
						    int time = output2Bytes;
						    int hours = (time / (60 * 60 * 60));
						    int minutes = (time / (60 * 60)) % 60;

						    int finalMinutes = (int)minutes;

						    std::stringstream ss;

						    ss << finalMinutes;
						    std::string minutesString = ss.str();

						    if (finalMinutes < 10)
							    minutesString = "0" + minutesString;

						    std::string newIdentifier = identifier;
						    newIdentifier.append(":%s");

						    finalOutput = StringFromFormat(newIdentifier.c_str(), hours, minutesString.c_str());
					    }
					    else
					    {
						    finalOutput = StringFromFormat(identifier.c_str(), output2Bytes);
					    }
				    }
				    else if (numBytes == 1)
				    {
					    u8 output1Byte = 0;

                                            PowerPC::TryReadResult<u8> result = HostTryReadU8(readAddress + 0x80000000, PowerPC::RequestedAddressSpace::Virtual);
                                            if (result)
                                                output1Byte = result.value;

					    finalOutput = StringFromFormat(identifier.c_str(), output1Byte);
				    }

				    if (finalOutput.length() == 0)
				    {
					    finalOutput = "N/A";
				    }
				    else
				    {
					    if (locHint == std::string::npos)
					    {
						    if (identifier.compare("%X") == 0 || identifier.compare("%x") == 0)
						    {
							    if (finalOutput.length() < 2)
							    {
								    finalOutput = "0" + finalOutput;
							    }
						    }
					    }
				    }

				    std::string completeOutput =
				        StringFromFormat("%s%s", currSectionOutput.c_str(), finalOutput.c_str());

				    RAMDisplay.append(completeOutput);

				    locNext = subLine.find("%", 0);
			    }

			    RAMDisplay.append("\n");
		    }
	    }

		return RAMDisplay;
	}

// NOTE: GPU Thread
std::string GetRTCDisplay()
{
  using ExpansionInterface::CEXIIPL;

  const time_t current_time = CEXIIPL::GetEmulatedTime(CEXIIPL::UNIX_EPOCH);
  const tm* const gm_time = gmtime(&current_time);

  std::ostringstream format_time;
  format_time << std::put_time(gm_time, "Date/Time: %c\n");
  return format_time.str();
}

void FrameUpdate()
{
  s_currentFrame++;
  if (!s_bPolled)
    s_currentLagCount++;

  if (IsRecordingInput())
  {
    s_totalFrames = s_currentFrame;
    s_totalLagCount = s_currentLagCount;
  }

  s_bPolled = false;
}

static void CheckMD5();
static void GetMD5();

// called when game is booting up, even if no movie is active,
// but potentially after BeginRecordingInput or PlayInput has been called.
// NOTE: EmuThread
void Init(const BootParameters& boot)
{
  if (std::holds_alternative<BootParameters::Disc>(boot.parameters))
    s_current_file_name = std::get<BootParameters::Disc>(boot.parameters).path;
  else
    s_current_file_name.clear();

  s_bPolled = false;
  s_bSaveConfig = false;
  if (IsPlayingInput())
  {
    ReadHeader();
    std::thread md5thread(CheckMD5);
    md5thread.detach();
    if (strncmp(tmpHeader.gameID.data(), SConfig::GetInstance().GetGameID().c_str(), 6))
    {
      PanicAlertFmtT("The recorded game ({0}) is not the same as the selected game ({1})",
                     tmpHeader.GetGameID(), SConfig::GetInstance().GetGameID());
      EndPlayInput(false);
    }
  }

  if (IsRecordingInput())
  {
    GetSettings();
    std::thread md5thread(GetMD5);
    md5thread.detach();
    s_tickCountAtLastInput = 0;
  }

  memset(&s_padState, 0, sizeof(s_padState));

  for (auto& disp : s_InputDisplay)
    disp.clear();

  if (!IsMovieActive())
  {
    s_bRecordingFromSaveState = false;
    s_rerecords = 0;
    s_currentByte = 0;
    s_currentFrame = 0;
    s_currentLagCount = 0;
    s_currentInputCount = 0;
  }
}

// NOTE: CPU Thread
void InputUpdate()
{
  s_currentInputCount++;
  if (IsRecordingInput())
  {
    s_totalInputCount = s_currentInputCount;
    s_totalTickCount += CoreTiming::GetTicks() - s_tickCountAtLastInput;
    s_tickCountAtLastInput = CoreTiming::GetTicks();
  }
}

// NOTE: CPU Thread
void SetPolledDevice()
{
  s_bPolled = true;
}

// NOTE: Host Thread
void SetReadOnly(bool bEnabled)
{
  if (s_bReadOnly != bEnabled)
    Core::DisplayMessage(bEnabled ? "Read-only mode." : "Read+Write mode.", 1000);

  s_bReadOnly = bEnabled;
}

bool IsRecordingInput()
{
  return (s_playMode == MODE_RECORDING);
}

bool IsRecordingInputFromSaveState()
{
  return s_bRecordingFromSaveState;
}

bool IsJustStartingRecordingInputFromSaveState()
{
  return IsRecordingInputFromSaveState() && s_currentFrame == 0;
}

bool IsJustStartingPlayingInputFromSaveState()
{
  return IsRecordingInputFromSaveState() && s_currentFrame == 1 && IsPlayingInput();
}

bool IsPlayingInput()
{
  return (s_playMode == MODE_PLAYING);
}

bool IsMovieActive()
{
  return s_playMode != MODE_NONE;
}

bool IsReadOnly()
{
  return s_bReadOnly;
}

u64 GetRecordingStartTime()
{
  return s_recordingStartTime;
}

u64 GetCurrentFrame()
{
  return s_currentFrame;
}

u64 GetTotalFrames()
{
  return s_totalFrames;
}

u64 GetCurrentInputCount()
{
  return s_currentInputCount;
}

u64 GetTotalInputCount()
{
  return s_totalInputCount;
}

u64 GetCurrentLagCount()
{
  return s_currentLagCount;
}

u64 GetTotalLagCount()
{
  return s_totalLagCount;
}

void SetClearSave(bool enabled)
{
  s_bClearSave = enabled;
}

void SignalDiscChange(const std::string& new_path)
{
  if (Movie::IsRecordingInput())
  {
    size_t size_of_path_without_filename = new_path.find_last_of("/\\") + 1;
    std::string filename = new_path.substr(size_of_path_without_filename);
    constexpr size_t maximum_length = sizeof(DTMHeader::discChange);
    if (filename.length() > maximum_length)
    {
      PanicAlertFmtT("The disc change to \"{0}\" could not be saved in the .dtm file.\n"
                     "The filename of the disc image must not be longer than 40 characters.",
                     filename);
    }
    s_discChange = filename;
    s_bDiscChange = true;
  }
}

void SetReset(bool reset)
{
  s_bReset = reset;
}

bool IsUsingPad(int controller)
{
  return s_controllers[controller] != ControllerType::None;
}

bool IsUsingBongo(int controller)
{
  return ((s_bongos & (1 << controller)) != 0);
}

bool IsUsingGBA(int controller)
{
  return s_controllers[controller] == ControllerType::GBA;
}

bool IsUsingWiimote(int wiimote)
{
  return s_wiimotes[wiimote];
}

bool IsConfigSaved()
{
  return s_bSaveConfig;
}

bool IsStartingFromClearSave()
{
  return s_bClearSave;
}

bool IsUsingMemcard(int memcard)
{
  return (s_memcards & (1 << memcard)) != 0;
}

bool IsNetPlayRecording()
{
  return s_bNetPlay;
}

// NOTE: Host Thread
void ChangePads()
{
  if (!Core::IsRunning())
    return;

  ControllerTypeArray controllers{};

  for (int i = 0; i < SerialInterface::MAX_SI_CHANNELS; ++i)
  {
    if (SConfig::GetInstance().m_SIDevice[i] == SerialInterface::SIDEVICE_GC_GBA_EMULATED)
      controllers[i] = ControllerType::GBA;
    else if (SerialInterface::SIDevice_IsGCController(SConfig::GetInstance().m_SIDevice[i]))
      controllers[i] = ControllerType::GC;
    else
      controllers[i] = ControllerType::None;
  }

  if (s_controllers == controllers)
    return;

  for (int i = 0; i < SerialInterface::MAX_SI_CHANNELS; ++i)
  {
    SerialInterface::SIDevices device = SerialInterface::SIDEVICE_NONE;
    if (IsUsingGBA(i))
    {
      device = SerialInterface::SIDEVICE_GC_GBA_EMULATED;
    }
    else if (IsUsingPad(i))
    {
      if (SerialInterface::SIDevice_IsGCController(SConfig::GetInstance().m_SIDevice[i]))
      {
        device = SConfig::GetInstance().m_SIDevice[i];
      }
      else
      {
        device = IsUsingBongo(i) ? SerialInterface::SIDEVICE_GC_TARUKONGA :
                                   SerialInterface::SIDEVICE_GC_CONTROLLER;
      }
    }

    SerialInterface::ChangeDevice(device, i);
  }
}

// NOTE: Host / Emu Threads
void ChangeWiiPads(bool instantly)
{
  WiimoteEnabledArray wiimotes{};

  for (int i = 0; i < MAX_WIIMOTES; ++i)
  {
    wiimotes[i] = WiimoteCommon::GetSource(i) != WiimoteSource::None;
  }

  // This is important for Wiimotes, because they can desync easily if they get re-activated
  if (instantly && s_wiimotes == wiimotes)
    return;

  const auto bt = WiiUtils::GetBluetoothEmuDevice();
  for (int i = 0; i < MAX_WIIMOTES; ++i)
  {
    const bool is_using_wiimote = IsUsingWiimote(i);

    WiimoteCommon::SetSource(i, is_using_wiimote ? WiimoteSource::Emulated : WiimoteSource::None);
    if (bt != nullptr)
      bt->AccessWiimoteByIndex(i)->Activate(is_using_wiimote);
  }
}

// NOTE: Host Thread
bool BeginRecordingInput(const ControllerTypeArray& controllers,
                         const WiimoteEnabledArray& wiimotes)
{
  if (s_playMode != MODE_NONE ||
      (controllers == ControllerTypeArray{} && wiimotes == WiimoteEnabledArray{}))
    return false;

  Core::RunAsCPUThread([controllers, wiimotes] {
    s_controllers = controllers;
    s_wiimotes = wiimotes;
    s_currentFrame = s_totalFrames = 0;
    s_currentLagCount = s_totalLagCount = 0;
    s_currentInputCount = s_totalInputCount = 0;
    s_totalTickCount = s_tickCountAtLastInput = 0;
    s_bongos = 0;
    s_memcards = 0;
    if (NetPlay::IsNetPlayRunning())
    {
      s_bNetPlay = true;
      s_recordingStartTime = ExpansionInterface::CEXIIPL::NetPlay_GetEmulatedTime();
    }
    else if (SConfig::GetInstance().bEnableCustomRTC)
    {
      s_recordingStartTime = SConfig::GetInstance().m_customRTCValue;
    }
    else
    {
      s_recordingStartTime = Common::Timer::GetLocalTimeSinceJan1970();
    }

    s_rerecords = 0;

    for (int i = 0; i < SerialInterface::MAX_SI_CHANNELS; ++i)
    {
      if (SConfig::GetInstance().m_SIDevice[i] == SerialInterface::SIDEVICE_GC_TARUKONGA)
        s_bongos |= (1 << i);
    }

    if (Core::IsRunningAndStarted())
    {
      const std::string save_path = File::GetUserPath(D_STATESAVES_IDX) + "dtm.sav";
      if (File::Exists(save_path))
        File::Delete(save_path);

      State::SaveAs(save_path);
      s_bRecordingFromSaveState = true;

      std::thread md5thread(GetMD5);
      md5thread.detach();
      GetSettings();
    }

    // Wiimotes cause desync issues if they're not reset before launching the game
    if (!Core::IsRunningAndStarted())
    {
      // This will also reset the wiimotes for gamecube games, but that shouldn't do anything
      Wiimote::ResetAllWiimotes();
    }

    s_playMode = MODE_RECORDING;
    s_author = SConfig::GetInstance().m_strMovieAuthor;
    s_temp_input.clear();

    s_currentByte = 0;

    if (Core::IsRunning())
      Core::UpdateWantDeterminism();
  });

  Core::DisplayMessage("Starting movie recording", 2000);
  return true;
}

static std::string Analog2DToString(u32 x, u32 y, const std::string& prefix, u32 range = 255)
{
  const u32 center = range / 2 + 1;

  if ((x <= 1 || x == center || x >= range) && (y <= 1 || y == center || y >= range))
  {
    if (x != center || y != center)
    {
      if (x != center && y != center)
      {
        return fmt::format("{}:{},{}", prefix, x < center ? "LEFT" : "RIGHT",
                           y < center ? "DOWN" : "UP");
      }

      if (x != center)
      {
        return fmt::format("{}:{}", prefix, x < center ? "LEFT" : "RIGHT");
      }

      return fmt::format("{}:{}", prefix, y < center ? "DOWN" : "UP");
    }

    return "";
  }

  return fmt::format("{}:{},{}", prefix, x, y);
}

static std::string Analog1DToString(u32 v, const std::string& prefix, u32 range = 255)
{
  if (v == 0)
    return "";

  if (v == range)
    return prefix;

  return fmt::format("{}:{}", prefix, v);
}

// NOTE: CPU Thread
static void SetInputDisplayString(ControllerState padState, int controllerID)
{
  std::string display_str = fmt::format("P{}:", controllerID + 1);

  if (padState.is_connected)
  {
    if (padState.A)
      display_str += " A";
    if (padState.B)
      display_str += " B";
    if (padState.X)
      display_str += " X";
    if (padState.Y)
      display_str += " Y";
    if (padState.Z)
      display_str += " Z";
    if (padState.Start)
      display_str += " START";

    if (padState.DPadUp)
      display_str += " UP";
    if (padState.DPadDown)
      display_str += " DOWN";
    if (padState.DPadLeft)
      display_str += " LEFT";
    if (padState.DPadRight)
      display_str += " RIGHT";
    if (padState.reset)
      display_str += " RESET";

    display_str += Analog1DToString(padState.TriggerL, " L");
    display_str += Analog1DToString(padState.TriggerR, " R");
    display_str += Analog2DToString(padState.AnalogStickX, padState.AnalogStickY, " ANA");
    display_str += Analog2DToString(padState.CStickX, padState.CStickY, " C");
  }
  else
  {
    display_str += " DISCONNECTED";
  }

  std::lock_guard guard(s_input_display_lock);
  s_InputDisplay[controllerID] = std::move(display_str);
}

// NOTE: CPU Thread
static void SetWiiInputDisplayString(int remoteID, const DataReportBuilder& rpt, int ext,
                                     const EncryptionKey& key)
{
  int controllerID = remoteID + 4;

  std::string display_str = fmt::format("R{}:", remoteID + 1);

  if (rpt.HasCore())
  {
    ButtonData buttons;
    rpt.GetCoreData(&buttons);

    if (buttons.left)
      display_str += " LEFT";
    if (buttons.right)
      display_str += " RIGHT";
    if (buttons.down)
      display_str += " DOWN";
    if (buttons.up)
      display_str += " UP";
    if (buttons.a)
      display_str += " A";
    if (buttons.b)
      display_str += " B";
    if (buttons.plus)
      display_str += " +";
    if (buttons.minus)
      display_str += " -";
    if (buttons.one)
      display_str += " 1";
    if (buttons.two)
      display_str += " 2";
    if (buttons.home)
      display_str += " HOME";
  }

  if (rpt.HasAccel())
  {
    AccelData accel_data;
    rpt.GetAccelData(&accel_data);

    // FYI: This will only print partial data for interleaved reports.

    display_str +=
        fmt::format(" ACC:{},{},{}", accel_data.value.x, accel_data.value.y, accel_data.value.z);
  }

  if (rpt.HasIR())
  {
    const u8* const ir_data = rpt.GetIRDataPtr();

    // TODO: This does not handle the different IR formats.

    const u16 x = ir_data[0] | ((ir_data[2] >> 4 & 0x3) << 8);
    const u16 y = ir_data[1] | ((ir_data[2] >> 6 & 0x3) << 8);
    display_str += fmt::format(" IR:{},{}", x, y);
  }

  // Nunchuk
  if (rpt.HasExt() && ext == ExtensionNumber::NUNCHUK)
  {
    const u8* const extData = rpt.GetExtDataPtr();

    Nunchuk::DataFormat nunchuk;
    memcpy(&nunchuk, extData, sizeof(nunchuk));
    key.Decrypt((u8*)&nunchuk, 0, sizeof(nunchuk));
    nunchuk.bt.hex = nunchuk.bt.hex ^ 0x3;

    const std::string accel = fmt::format(" N-ACC:{},{},{}", nunchuk.GetAccelX(),
                                          nunchuk.GetAccelY(), nunchuk.GetAccelZ());

    if (nunchuk.bt.c)
      display_str += " C";
    if (nunchuk.bt.z)
      display_str += " Z";
    display_str += accel;
    display_str += Analog2DToString(nunchuk.jx, nunchuk.jy, " ANA");
  }

  // Classic controller
  if (rpt.HasExt() && ext == ExtensionNumber::CLASSIC)
  {
    const u8* const extData = rpt.GetExtDataPtr();

    Classic::DataFormat cc;
    memcpy(&cc, extData, sizeof(cc));
    key.Decrypt((u8*)&cc, 0, sizeof(cc));
    cc.bt.hex = cc.bt.hex ^ 0xFFFF;

    if (cc.bt.dpad_left)
      display_str += " LEFT";
    if (cc.bt.dpad_right)
      display_str += " RIGHT";
    if (cc.bt.dpad_down)
      display_str += " DOWN";
    if (cc.bt.dpad_up)
      display_str += " UP";
    if (cc.bt.a)
      display_str += " A";
    if (cc.bt.b)
      display_str += " B";
    if (cc.bt.x)
      display_str += " X";
    if (cc.bt.y)
      display_str += " Y";
    if (cc.bt.zl)
      display_str += " ZL";
    if (cc.bt.zr)
      display_str += " ZR";
    if (cc.bt.plus)
      display_str += " +";
    if (cc.bt.minus)
      display_str += " -";
    if (cc.bt.home)
      display_str += " HOME";

    display_str += Analog1DToString(cc.GetLeftTrigger().value, " L", 31);
    display_str += Analog1DToString(cc.GetRightTrigger().value, " R", 31);

    const auto left_stick = cc.GetLeftStick().value;
    display_str += Analog2DToString(left_stick.x, left_stick.y, " ANA", 63);

    const auto right_stick = cc.GetRightStick().value;
    display_str += Analog2DToString(right_stick.x, right_stick.y, " R-ANA", 31);
  }

  std::lock_guard guard(s_input_display_lock);
  s_InputDisplay[controllerID] = std::move(display_str);
}

// NOTE: CPU Thread
void CheckPadStatus(const GCPadStatus* PadStatus, int controllerID)
{
  s_padState.A = ((PadStatus->button & PAD_BUTTON_A) != 0);
  s_padState.B = ((PadStatus->button & PAD_BUTTON_B) != 0);
  s_padState.X = ((PadStatus->button & PAD_BUTTON_X) != 0);
  s_padState.Y = ((PadStatus->button & PAD_BUTTON_Y) != 0);
  s_padState.Z = ((PadStatus->button & PAD_TRIGGER_Z) != 0);
  s_padState.Start = ((PadStatus->button & PAD_BUTTON_START) != 0);

  s_padState.DPadUp = ((PadStatus->button & PAD_BUTTON_UP) != 0);
  s_padState.DPadDown = ((PadStatus->button & PAD_BUTTON_DOWN) != 0);
  s_padState.DPadLeft = ((PadStatus->button & PAD_BUTTON_LEFT) != 0);
  s_padState.DPadRight = ((PadStatus->button & PAD_BUTTON_RIGHT) != 0);

  s_padState.L = ((PadStatus->button & PAD_TRIGGER_L) != 0);
  s_padState.R = ((PadStatus->button & PAD_TRIGGER_R) != 0);
  s_padState.TriggerL = PadStatus->triggerLeft;
  s_padState.TriggerR = PadStatus->triggerRight;

  s_padState.AnalogStickX = PadStatus->stickX;
  s_padState.AnalogStickY = PadStatus->stickY;

  s_padState.CStickX = PadStatus->substickX;
  s_padState.CStickY = PadStatus->substickY;

  s_padState.is_connected = PadStatus->isConnected;

  s_padState.get_origin = (PadStatus->button & PAD_GET_ORIGIN) != 0;

  s_padState.disc = s_bDiscChange;
  s_bDiscChange = false;
  s_padState.reset = s_bReset;
  s_bReset = false;

  SetInputDisplayString(s_padState, controllerID);
}

// NOTE: CPU Thread
void RecordInput(const GCPadStatus* PadStatus, int controllerID)
{
  if (!IsRecordingInput() || !IsUsingPad(controllerID))
    return;

  CheckPadStatus(PadStatus, controllerID);

  s_temp_input.resize(s_currentByte + sizeof(ControllerState));
  memcpy(&s_temp_input[s_currentByte], &s_padState, sizeof(ControllerState));
  s_currentByte += sizeof(ControllerState);
}

// NOTE: CPU Thread
void CheckWiimoteStatus(int wiimote, const DataReportBuilder& rpt, int ext,
                        const EncryptionKey& key)
{
  SetWiiInputDisplayString(wiimote, rpt, ext, key);

  if (IsRecordingInput())
    RecordWiimote(wiimote, rpt.GetDataPtr(), rpt.GetDataSize());
}

void RecordWiimote(int wiimote, const u8* data, u8 size)
{
  if (!IsRecordingInput() || !IsUsingWiimote(wiimote))
    return;

  InputUpdate();
  s_temp_input.resize(s_currentByte + size + 1);
  s_temp_input[s_currentByte++] = size;
  memcpy(&s_temp_input[s_currentByte], data, size);
  s_currentByte += size;
}

// NOTE: EmuThread / Host Thread
void ReadHeader()
{
  for (int i = 0; i < 4; ++i)
  {
    if (tmpHeader.GBAControllers & (1 << i))
      s_controllers[i] = ControllerType::GBA;
    else if (tmpHeader.controllers & (1 << i))
      s_controllers[i] = ControllerType::GC;
    else
      s_controllers[i] = ControllerType::None;
    s_wiimotes[i] = (tmpHeader.controllers & (1 << (i + 4))) != 0;
  }
  s_recordingStartTime = tmpHeader.recordingStartTime;
  if (s_rerecords < tmpHeader.numRerecords)
    s_rerecords = tmpHeader.numRerecords;

  if (tmpHeader.bSaveConfig)
  {
    s_bSaveConfig = true;
    Config::AddLayer(ConfigLoaders::GenerateMovieConfigLoader(&tmpHeader));
    SConfig::GetInstance().bJITFollowBranch = tmpHeader.bFollowBranch;
    s_bClearSave = tmpHeader.bClearSave;
    s_memcards = tmpHeader.memcards;
    s_bongos = tmpHeader.bongos;
    s_bNetPlay = tmpHeader.bNetPlay;
    s_revision = tmpHeader.revision;
  }
  else
  {
    GetSettings();
  }

  s_discChange = {tmpHeader.discChange.begin(), tmpHeader.discChange.end()};
  s_author = {tmpHeader.author.begin(), tmpHeader.author.end()};
  s_MD5 = tmpHeader.md5;
  s_DSPiromHash = tmpHeader.DSPiromHash;
  s_DSPcoefHash = tmpHeader.DSPcoefHash;
}

// NOTE: Host Thread
bool PlayInput(const std::string& movie_path, std::optional<std::string>* savestate_path)
{
  if (s_playMode != MODE_NONE)
    return false;

  File::IOFile recording_file(movie_path, "rb");
  if (!recording_file.ReadArray(&tmpHeader, 1))
    return false;

  if (!IsMovieHeader(tmpHeader.filetype))
  {
    PanicAlertFmtT("Invalid recording file");
    return false;
  }

  ReadHeader();
  s_totalFrames = tmpHeader.frameCount;
  s_totalLagCount = tmpHeader.lagCount;
  s_totalInputCount = tmpHeader.inputCount;
  s_totalTickCount = tmpHeader.tickCount;
  s_currentFrame = 0;
  s_currentLagCount = 0;
  s_currentInputCount = 0;

  s_playMode = MODE_PLAYING;

  // Wiimotes cause desync issues if they're not reset before launching the game
  Wiimote::ResetAllWiimotes();

  Core::UpdateWantDeterminism();

  s_temp_input.resize(recording_file.GetSize() - 256);
  recording_file.ReadBytes(s_temp_input.data(), s_temp_input.size());
  s_currentByte = 0;
  recording_file.Close();

  // Load savestate (and skip to frame data)
  if (tmpHeader.bFromSaveState && savestate_path)
  {
    const std::string savestate_path_temp = movie_path + ".sav";
    if (File::Exists(savestate_path_temp))
      *savestate_path = savestate_path_temp;
    s_bRecordingFromSaveState = true;
    Movie::LoadInput(movie_path);
  }

  return true;
}

void DoState(PointerWrap& p)
{
  // many of these could be useful to save even when no movie is active,
  // and the data is tiny, so let's just save it regardless of movie state.
  p.Do(s_currentFrame);
  p.Do(s_currentByte);
  p.Do(s_currentLagCount);
  p.Do(s_currentInputCount);
  p.Do(s_bPolled);
  p.Do(s_tickCountAtLastInput);
  // other variables (such as s_totalBytes and s_totalFrames) are set in LoadInput
}

// NOTE: Host Thread
void LoadInput(const std::string& movie_path)
{
  File::IOFile t_record;
  if (!t_record.Open(movie_path, "r+b"))
  {
    PanicAlertFmtT("Failed to read {0}", movie_path);
    EndPlayInput(false);
    return;
  }

  t_record.ReadArray(&tmpHeader, 1);

  if (!IsMovieHeader(tmpHeader.filetype))
  {
    PanicAlertFmtT("Savestate movie {0} is corrupted, movie recording stopping...", movie_path);
    EndPlayInput(false);
    return;
  }
  ReadHeader();
  if (!s_bReadOnly)
  {
    s_rerecords++;
    tmpHeader.numRerecords = s_rerecords;
    t_record.Seek(0, SEEK_SET);
    t_record.WriteArray(&tmpHeader, 1);
  }

  ChangePads();
  if (SConfig::GetInstance().bWii)
    ChangeWiiPads(true);

  u64 totalSavedBytes = t_record.GetSize() - 256;

  bool afterEnd = false;
  // This can only happen if the user manually deletes data from the dtm.
  if (s_currentByte > totalSavedBytes)
  {
    PanicAlertFmtT(
        "Warning: You loaded a save whose movie ends before the current frame in the save "
        "(byte {0} < {1}) (frame {2} < {3}). You should load another save before continuing.",
        totalSavedBytes + 256, s_currentByte + 256, tmpHeader.frameCount, s_currentFrame);
    afterEnd = true;
  }

  if (!s_bReadOnly || s_temp_input.empty())
  {
    s_totalFrames = tmpHeader.frameCount;
    s_totalLagCount = tmpHeader.lagCount;
    s_totalInputCount = tmpHeader.inputCount;
    s_totalTickCount = s_tickCountAtLastInput = tmpHeader.tickCount;

    s_temp_input.resize(static_cast<size_t>(totalSavedBytes));
    t_record.ReadBytes(s_temp_input.data(), s_temp_input.size());
  }
  else if (s_currentByte > 0)
  {
    if (s_currentByte > totalSavedBytes)
    {
    }
    else if (s_currentByte > s_temp_input.size())
    {
      afterEnd = true;
      PanicAlertFmtT(
          "Warning: You loaded a save that's after the end of the current movie. (byte {0} "
          "> {1}) (input {2} > {3}). You should load another save before continuing, or load "
          "this state with read-only mode off.",
          s_currentByte + 256, s_temp_input.size() + 256, s_currentInputCount, s_totalInputCount);
    }
    else if (s_currentByte > 0 && !s_temp_input.empty())
    {
      // verify identical from movie start to the save's current frame
      std::vector<u8> movInput(s_currentByte);
      t_record.ReadArray(movInput.data(), movInput.size());

      const auto result = std::mismatch(movInput.begin(), movInput.end(), s_temp_input.begin());

      if (result.first != movInput.end())
      {
        const ptrdiff_t mismatch_index = std::distance(movInput.begin(), result.first);

        // this is a "you did something wrong" alert for the user's benefit.
        // we'll try to say what's going on in excruciating detail, otherwise the user might not
        // believe us.
        if (IsUsingWiimote(0))
        {
          const size_t byte_offset = static_cast<size_t>(mismatch_index) + sizeof(DTMHeader);

          // TODO: more detail
          PanicAlertFmtT("Warning: You loaded a save whose movie mismatches on byte {0} ({1:#x}). "
                         "You should load another save before continuing, or load this state with "
                         "read-only mode off. Otherwise you'll probably get a desync.",
                         byte_offset, byte_offset);

          std::copy(movInput.begin(), movInput.end(), s_temp_input.begin());
        }
        else
        {
          const ptrdiff_t frame = mismatch_index / sizeof(ControllerState);
          ControllerState curPadState;
          memcpy(&curPadState, &s_temp_input[frame * sizeof(ControllerState)],
                 sizeof(ControllerState));
          ControllerState movPadState;
          memcpy(&movPadState, &s_temp_input[frame * sizeof(ControllerState)],
                 sizeof(ControllerState));
          PanicAlertFmtT(
              "Warning: You loaded a save whose movie mismatches on frame {0}. You should load "
              "another save before continuing, or load this state with read-only mode off. "
              "Otherwise you'll probably get a desync.\n\n"
              "More information: The current movie is {1} frames long and the savestate's movie "
              "is {2} frames long.\n\n"
              "On frame {3}, the current movie presses:\n"
              "Start={4}, A={5}, B={6}, X={7}, Y={8}, Z={9}, DUp={10}, DDown={11}, DLeft={12}, "
              "DRight={13}, L={14}, R={15}, LT={16}, RT={17}, AnalogX={18}, AnalogY={19}, CX={20}, "
              "CY={21}, Connected={22}"
              "\n\n"
              "On frame {23}, the savestate's movie presses:\n"
              "Start={24}, A={25}, B={26}, X={27}, Y={28}, Z={29}, DUp={30}, DDown={31}, "
              "DLeft={32}, DRight={33}, L={34}, R={35}, LT={36}, RT={37}, AnalogX={38}, "
              "AnalogY={39}, CX={40}, CY={41}, Connected={42}",
              frame, s_totalFrames, tmpHeader.frameCount, frame, curPadState.Start, curPadState.A,
              curPadState.B, curPadState.X, curPadState.Y, curPadState.Z, curPadState.DPadUp,
              curPadState.DPadDown, curPadState.DPadLeft, curPadState.DPadRight, curPadState.L,
              curPadState.R, curPadState.TriggerL, curPadState.TriggerR, curPadState.AnalogStickX,
              curPadState.AnalogStickY, curPadState.CStickX, curPadState.CStickY,
              curPadState.is_connected, frame, movPadState.Start, movPadState.A, movPadState.B,
              movPadState.X, movPadState.Y, movPadState.Z, movPadState.DPadUp, movPadState.DPadDown,
              movPadState.DPadLeft, movPadState.DPadRight, movPadState.L, movPadState.R,
              movPadState.TriggerL, movPadState.TriggerR, movPadState.AnalogStickX,
              movPadState.AnalogStickY, movPadState.CStickX, movPadState.CStickY,
              curPadState.is_connected);
        }
      }
    }
  }
  t_record.Close();

  s_bSaveConfig = tmpHeader.bSaveConfig;

  if (!afterEnd)
  {
    if (s_bReadOnly)
    {
      if (s_playMode != MODE_PLAYING)
      {
        s_playMode = MODE_PLAYING;
        Core::UpdateWantDeterminism();
        Core::DisplayMessage("Switched to playback", 2000);
      }
    }
    else
    {
      if (s_playMode != MODE_RECORDING)
      {
        s_playMode = MODE_RECORDING;
        Core::UpdateWantDeterminism();
        Core::DisplayMessage("Switched to recording", 2000);
      }
    }
  }
  else
  {
    EndPlayInput(false);
  }
}

// NOTE: CPU Thread
static void CheckInputEnd()
{
  if (s_currentByte >= s_temp_input.size() ||
      (CoreTiming::GetTicks() > s_totalTickCount && !IsRecordingInputFromSaveState()))
  {
    EndPlayInput(!s_bReadOnly);
  }
}

// NOTE: CPU Thread
void PlayController(GCPadStatus* PadStatus, int controllerID)
{
  // Correct playback is entirely dependent on the emulator polling the controllers
  // in the same order done during recording
  if (!IsPlayingInput() || !IsUsingPad(controllerID) || s_temp_input.empty())
    return;

  if (s_currentByte + sizeof(ControllerState) > s_temp_input.size())
  {
    PanicAlertFmtT("Premature movie end in PlayController. {0} + {1} > {2}", s_currentByte,
                   sizeof(ControllerState), s_temp_input.size());
    EndPlayInput(!s_bReadOnly);
    return;
  }

  memcpy(&s_padState, &s_temp_input[s_currentByte], sizeof(ControllerState));
  s_currentByte += sizeof(ControllerState);

  PadStatus->isConnected = s_padState.is_connected;

  PadStatus->triggerLeft = s_padState.TriggerL;
  PadStatus->triggerRight = s_padState.TriggerR;

  PadStatus->stickX = s_padState.AnalogStickX;
  PadStatus->stickY = s_padState.AnalogStickY;

  PadStatus->substickX = s_padState.CStickX;
  PadStatus->substickY = s_padState.CStickY;

  PadStatus->button = 0;
  PadStatus->button |= PAD_USE_ORIGIN;

  if (s_padState.A)
  {
    PadStatus->button |= PAD_BUTTON_A;
    PadStatus->analogA = 0xFF;
  }
  if (s_padState.B)
  {
    PadStatus->button |= PAD_BUTTON_B;
    PadStatus->analogB = 0xFF;
  }
  if (s_padState.X)
    PadStatus->button |= PAD_BUTTON_X;
  if (s_padState.Y)
    PadStatus->button |= PAD_BUTTON_Y;
  if (s_padState.Z)
    PadStatus->button |= PAD_TRIGGER_Z;
  if (s_padState.Start)
    PadStatus->button |= PAD_BUTTON_START;

  if (s_padState.DPadUp)
    PadStatus->button |= PAD_BUTTON_UP;
  if (s_padState.DPadDown)
    PadStatus->button |= PAD_BUTTON_DOWN;
  if (s_padState.DPadLeft)
    PadStatus->button |= PAD_BUTTON_LEFT;
  if (s_padState.DPadRight)
    PadStatus->button |= PAD_BUTTON_RIGHT;

  if (s_padState.L)
    PadStatus->button |= PAD_TRIGGER_L;
  if (s_padState.R)
    PadStatus->button |= PAD_TRIGGER_R;

  if (s_padState.get_origin)
    PadStatus->button |= PAD_GET_ORIGIN;

  if (s_padState.disc)
  {
    Core::RunAsCPUThread([] {
      if (!DVDInterface::AutoChangeDisc())
      {
        CPU::Break();
        PanicAlertFmtT("Change the disc to {0}", s_discChange);
      }
    });
  }

  if (s_padState.reset)
    ProcessorInterface::ResetButton_Tap();

  SetInputDisplayString(s_padState, controllerID);
  CheckInputEnd();
}

// NOTE: CPU Thread
bool PlayWiimote(int wiimote, WiimoteCommon::DataReportBuilder& rpt, int ext,
                 const EncryptionKey& key)
{
  if (!IsPlayingInput() || !IsUsingWiimote(wiimote) || s_temp_input.empty())
    return false;

  if (s_currentByte > s_temp_input.size())
  {
    PanicAlertFmtT("Premature movie end in PlayWiimote. {0} > {1}", s_currentByte,
                   s_temp_input.size());
    EndPlayInput(!s_bReadOnly);
    return false;
  }

  const u8 size = rpt.GetDataSize();
  const u8 sizeInMovie = s_temp_input[s_currentByte];

  if (size != sizeInMovie)
  {
    PanicAlertFmtT(
        "Fatal desync. Aborting playback. (Error in PlayWiimote: {0} != {1}, byte {2}.){3}",
        sizeInMovie, size, s_currentByte,
        (s_controllers == ControllerTypeArray{}) ?
            " Try re-creating the recording with all GameCube controllers "
            "disabled (in Configure > GameCube > Device Settings)." :
            "");
    EndPlayInput(!s_bReadOnly);
    return false;
  }

  s_currentByte++;

  if (s_currentByte + size > s_temp_input.size())
  {
    PanicAlertFmtT("Premature movie end in PlayWiimote. {0} + {1} > {2}", s_currentByte, size,
                   s_temp_input.size());
    EndPlayInput(!s_bReadOnly);
    return false;
  }

  memcpy(rpt.GetDataPtr(), &s_temp_input[s_currentByte], size);
  s_currentByte += size;

  s_currentInputCount++;

  CheckInputEnd();
  return true;
}

// NOTE: Host / EmuThread / CPU Thread
void EndPlayInput(bool cont)
{
  if (cont)
  {
    // If !IsMovieActive(), changing s_playMode requires calling UpdateWantDeterminism
    ASSERT(IsMovieActive());

    s_playMode = MODE_RECORDING;
    Core::DisplayMessage("Reached movie end. Resuming recording.", 2000);
  }
  else if (s_playMode != MODE_NONE)
  {
    // We can be called by EmuThread during boot (CPU::State::PowerDown)
    bool was_running = Core::IsRunningAndStarted() && !CPU::IsStepping();
    if (was_running)
      CPU::Break();
    s_rerecords = 0;
    s_currentByte = 0;
    s_playMode = MODE_NONE;
    Core::DisplayMessage("Movie End.", 2000);
    s_bRecordingFromSaveState = false;
    // we don't clear these things because otherwise we can't resume playback if we load a movie
    // state later
    // s_totalFrames = s_totalBytes = 0;
    // delete tmpInput;
    // tmpInput = nullptr;

    Core::QueueHostJob([=] {
      Core::UpdateWantDeterminism();
      if (was_running && !SConfig::GetInstance().m_PauseMovie)
        CPU::EnableStepping(false);
    });
  }
}

// NOTE: Save State + Host Thread
void SaveRecording(const std::string& filename)
{
  File::IOFile save_record(filename, "wb");
  // Create the real header now and write it
  DTMHeader header;
  memset(&header, 0, sizeof(DTMHeader));

  header.filetype[0] = 'D';
  header.filetype[1] = 'T';
  header.filetype[2] = 'M';
  header.filetype[3] = 0x1A;
  strncpy(header.gameID.data(), SConfig::GetInstance().GetGameID().c_str(), 6);
  header.bWii = SConfig::GetInstance().bWii;
  header.bFollowBranch = SConfig::GetInstance().bJITFollowBranch;
  header.controllers = 0;
  header.GBAControllers = 0;
  for (int i = 0; i < 4; ++i)
  {
    if (IsUsingGBA(i))
      header.GBAControllers |= 1 << i;
    if (IsUsingPad(i))
      header.controllers |= 1 << i;
    if (IsUsingWiimote(i) && SConfig::GetInstance().bWii)
      header.controllers |= 1 << (i + 4);
  }

  header.bFromSaveState = s_bRecordingFromSaveState;
  header.frameCount = s_totalFrames;
  header.lagCount = s_totalLagCount;
  header.inputCount = s_totalInputCount;
  header.numRerecords = s_rerecords;
  header.recordingStartTime = s_recordingStartTime;

  header.bSaveConfig = true;
  ConfigLoaders::SaveToDTM(&header);
  header.memcards = s_memcards;
  header.bClearSave = s_bClearSave;
  header.bNetPlay = s_bNetPlay;
  strncpy(header.discChange.data(), s_discChange.c_str(), header.discChange.size());
  strncpy(header.author.data(), s_author.c_str(), header.author.size());
  header.md5 = s_MD5;
  header.bongos = s_bongos;
  header.revision = s_revision;
  header.DSPiromHash = s_DSPiromHash;
  header.DSPcoefHash = s_DSPcoefHash;
  header.tickCount = s_totalTickCount;

  // TODO
  header.uniqueID = 0;
  // header.audioEmulator;

  save_record.WriteArray(&header, 1);

  bool success = save_record.WriteBytes(s_temp_input.data(), s_temp_input.size());

  if (success && s_bRecordingFromSaveState)
  {
    std::string stateFilename = filename + ".sav";
    success = File::Copy(File::GetUserPath(D_STATESAVES_IDX) + "dtm.sav", stateFilename);
  }

  if (success)
    Core::DisplayMessage(fmt::format("DTM {} saved", filename), 2000);
  else
    Core::DisplayMessage(fmt::format("Failed to save {}", filename), 2000);
}

void SetGCInputManip(GCManipFunction func)
{
  s_gc_manip_func = std::move(func);
}
void SetWiiInputManip(WiiManipFunction func)
{
  s_wii_manip_func = std::move(func);
}


// NOTE: CPU Thread
void CallGCInputManip(GCPadStatus* PadStatus, int controllerID)
{
  if (s_gc_manip_func)
    s_gc_manip_func(PadStatus, controllerID);

  
}
// NOTE: CPU Thread
void CallWiiInputManip(DataReportBuilder& rpt, int controllerID, int ext, const EncryptionKey& key)
{
  if (s_wii_manip_func)
    s_wii_manip_func(rpt, controllerID, ext, key);
}

// NOTE: GPU Thread
void SetGraphicsConfig()
{
  g_Config.bEFBAccessEnable = tmpHeader.bEFBAccessEnable;
  g_Config.bSkipEFBCopyToRam = tmpHeader.bSkipEFBCopyToRam;
  g_Config.bEFBEmulateFormatChanges = tmpHeader.bEFBEmulateFormatChanges;
  g_Config.bImmediateXFB = tmpHeader.bImmediateXFB;
  g_Config.bSkipXFBCopyToRam = tmpHeader.bSkipXFBCopyToRam;
}

// NOTE: EmuThread / Host Thread
void GetSettings()
{
  const bool slot_a_has_raw_memcard =
      SConfig::GetInstance().m_EXIDevice[0] == ExpansionInterface::EXIDEVICE_MEMORYCARD;
  const bool slot_a_has_gci_folder =
      SConfig::GetInstance().m_EXIDevice[0] == ExpansionInterface::EXIDEVICE_MEMORYCARDFOLDER;
  const bool slot_b_has_raw_memcard =
      SConfig::GetInstance().m_EXIDevice[1] == ExpansionInterface::EXIDEVICE_MEMORYCARD;
  const bool slot_b_has_gci_folder =
      SConfig::GetInstance().m_EXIDevice[1] == ExpansionInterface::EXIDEVICE_MEMORYCARDFOLDER;

  s_bSaveConfig = true;
  s_bNetPlay = NetPlay::IsNetPlayRunning();
  if (SConfig::GetInstance().bWii)
  {
    u64 title_id = SConfig::GetInstance().GetTitleID();
    s_bClearSave = !File::Exists(Common::GetTitleDataPath(title_id, Common::FROM_SESSION_ROOT) +
                                 "/banner.bin");
  }
  else
  {
    const auto gci_folder_has_saves = [](int card_index) {
      const auto [path, migrate] = ExpansionInterface::CEXIMemoryCard::GetGCIFolderPath(
          card_index, ExpansionInterface::AllowMovieFolder::No);
      const u64 number_of_saves = File::ScanDirectoryTree(path, false).size;
      return number_of_saves > 0;
    };

    s_bClearSave =
        !(slot_a_has_raw_memcard && File::Exists(Config::Get(Config::MAIN_MEMCARD_A_PATH))) &&
        !(slot_b_has_raw_memcard && File::Exists(Config::Get(Config::MAIN_MEMCARD_B_PATH))) &&
        !(slot_a_has_gci_folder && gci_folder_has_saves(0)) &&
        !(slot_b_has_gci_folder && gci_folder_has_saves(1));
  }
  s_memcards |= (slot_a_has_raw_memcard || slot_a_has_gci_folder) << 0;
  s_memcards |= (slot_b_has_raw_memcard || slot_b_has_gci_folder) << 1;

  s_revision = ConvertGitRevisionToBytes(Common::scm_rev_git_str);

  if (!Config::Get(Config::MAIN_DSP_HLE))
  {
    std::string irom_file = File::GetUserPath(D_GCUSER_IDX) + DSP_IROM;
    std::string coef_file = File::GetUserPath(D_GCUSER_IDX) + DSP_COEF;

    if (!File::Exists(irom_file))
      irom_file = File::GetSysDirectory() + GC_SYS_DIR DIR_SEP DSP_IROM;
    if (!File::Exists(coef_file))
      coef_file = File::GetSysDirectory() + GC_SYS_DIR DIR_SEP DSP_COEF;
    std::vector<u16> irom(DSP::DSP_IROM_SIZE);
    File::IOFile file_irom(irom_file, "rb");

    file_irom.ReadArray(irom.data(), irom.size());
    file_irom.Close();
    for (u16& entry : irom)
      entry = Common::swap16(entry);

    std::vector<u16> coef(DSP::DSP_COEF_SIZE);
    File::IOFile file_coef(coef_file, "rb");

    file_coef.ReadArray(coef.data(), coef.size());
    file_coef.Close();
    for (u16& entry : coef)
      entry = Common::swap16(entry);
    s_DSPiromHash =
        Common::HashAdler32(reinterpret_cast<u8*>(irom.data()), DSP::DSP_IROM_BYTE_SIZE);
    s_DSPcoefHash =
        Common::HashAdler32(reinterpret_cast<u8*>(coef.data()), DSP::DSP_COEF_BYTE_SIZE);
  }
  else
  {
    s_DSPiromHash = 0;
    s_DSPcoefHash = 0;
  }
}

static const mbedtls_md_info_t* s_md5_info = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);

// NOTE: Entrypoint for own thread
static void CheckMD5()
{
  if (s_current_file_name.empty())
    return;

  for (int i = 0, n = 0; i < 16; ++i)
  {
    if (tmpHeader.md5[i] != 0)
      continue;
    n++;
    if (n == 16)
      return;
  }
  Core::DisplayMessage("Verifying checksum...", 2000);

  std::array<u8, 16> game_md5;
  mbedtls_md_file(s_md5_info, s_current_file_name.c_str(), game_md5.data());

  if (game_md5 == s_MD5)
    Core::DisplayMessage("Checksum of current game matches the recorded game.", 2000);
  else
    Core::DisplayMessage("Checksum of current game does not match the recorded game!", 3000);
}

// NOTE: Entrypoint for own thread
static void GetMD5()
{
  if (s_current_file_name.empty())
    return;

  Core::DisplayMessage("Calculating checksum of game file...", 2000);
  mbedtls_md_file(s_md5_info, s_current_file_name.c_str(), s_MD5.data());
  Core::DisplayMessage("Finished calculating checksum.", 2000);
}

// NOTE: EmuThread
void Shutdown()
{
  s_currentInputCount = s_totalInputCount = s_totalFrames = s_tickCountAtLastInput = 0;
  s_temp_input.clear();
}
}  // namespace Movie
