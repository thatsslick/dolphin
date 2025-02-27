// Copyright 2021 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/CheatSearch.h"

#include <cassert>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "Common/Align.h"
#include "Common/BitUtils.h"
#include "Common/StringUtil.h"

#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"

Cheats::DataType Cheats::GetDataType(const Cheats::SearchValue& value)
{
  // sanity checks that our enum matches with our std::variant
  using should_be_u8 = std::remove_cv_t<
      std::remove_reference_t<decltype(std::get<static_cast<int>(DataType::U8)>(value.m_value))>>;
  using should_be_u16 = std::remove_cv_t<
      std::remove_reference_t<decltype(std::get<static_cast<int>(DataType::U16)>(value.m_value))>>;
  using should_be_u32 = std::remove_cv_t<
      std::remove_reference_t<decltype(std::get<static_cast<int>(DataType::U32)>(value.m_value))>>;
  using should_be_u64 = std::remove_cv_t<
      std::remove_reference_t<decltype(std::get<static_cast<int>(DataType::U64)>(value.m_value))>>;
  using should_be_s8 = std::remove_cv_t<
      std::remove_reference_t<decltype(std::get<static_cast<int>(DataType::S8)>(value.m_value))>>;
  using should_be_s16 = std::remove_cv_t<
      std::remove_reference_t<decltype(std::get<static_cast<int>(DataType::S16)>(value.m_value))>>;
  using should_be_s32 = std::remove_cv_t<
      std::remove_reference_t<decltype(std::get<static_cast<int>(DataType::S32)>(value.m_value))>>;
  using should_be_s64 = std::remove_cv_t<
      std::remove_reference_t<decltype(std::get<static_cast<int>(DataType::S64)>(value.m_value))>>;
  using should_be_f32 = std::remove_cv_t<
      std::remove_reference_t<decltype(std::get<static_cast<int>(DataType::F32)>(value.m_value))>>;
  using should_be_f64 = std::remove_cv_t<
      std::remove_reference_t<decltype(std::get<static_cast<int>(DataType::F64)>(value.m_value))>>;
  static_assert(std::is_same_v<u8, should_be_u8>);
  static_assert(std::is_same_v<u16, should_be_u16>);
  static_assert(std::is_same_v<u32, should_be_u32>);
  static_assert(std::is_same_v<u64, should_be_u64>);
  static_assert(std::is_same_v<s8, should_be_s8>);
  static_assert(std::is_same_v<s16, should_be_s16>);
  static_assert(std::is_same_v<s32, should_be_s32>);
  static_assert(std::is_same_v<s64, should_be_s64>);
  static_assert(std::is_same_v<float, should_be_f32>);
  static_assert(std::is_same_v<double, should_be_f64>);

  return static_cast<DataType>(value.m_value.index());
}

template <typename T>
static std::vector<u8> ToByteVector(const T& val)
{
  static_assert(std::is_trivially_copyable_v<T>);
  const auto* const begin = reinterpret_cast<const u8*>(&val);
  const auto* const end = begin + sizeof(T);
  return {begin, end};
}

std::vector<u8> Cheats::GetValueAsByteVector(const Cheats::SearchValue& value)
{
  DataType type = GetDataType(value);
  switch (type)
  {
  case Cheats::DataType::U8:
    return {std::get<u8>(value.m_value)};
  case Cheats::DataType::U16:
    return ToByteVector(Common::swap16(std::get<u16>(value.m_value)));
  case Cheats::DataType::U32:
    return ToByteVector(Common::swap32(std::get<u32>(value.m_value)));
  case Cheats::DataType::U64:
    return ToByteVector(Common::swap64(std::get<u64>(value.m_value)));
  case Cheats::DataType::S8:
    return {Common::BitCast<u8>(std::get<s8>(value.m_value))};
  case Cheats::DataType::S16:
    return ToByteVector(Common::swap16(Common::BitCast<u16>(std::get<s16>(value.m_value))));
  case Cheats::DataType::S32:
    return ToByteVector(Common::swap32(Common::BitCast<u32>(std::get<s32>(value.m_value))));
  case Cheats::DataType::S64:
    return ToByteVector(Common::swap64(Common::BitCast<u64>(std::get<s64>(value.m_value))));
  case Cheats::DataType::F32:
    return ToByteVector(Common::swap32(Common::BitCast<u32>(std::get<float>(value.m_value))));
  case Cheats::DataType::F64:
    return ToByteVector(Common::swap64(Common::BitCast<u64>(std::get<double>(value.m_value))));
  default:
    assert(0);
    return {};
  }
}

namespace
{
template <typename T>
static PowerPC::TryReadResult<T>
TryReadValueFromEmulatedMemory(u32 addr, PowerPC::RequestedAddressSpace space);

template <>
PowerPC::TryReadResult<u8> TryReadValueFromEmulatedMemory(u32 addr,
                                                          PowerPC::RequestedAddressSpace space)
{
  return PowerPC::HostTryReadU8(addr, space);
}

template <>
PowerPC::TryReadResult<u16> TryReadValueFromEmulatedMemory(u32 addr,
                                                           PowerPC::RequestedAddressSpace space)
{
  return PowerPC::HostTryReadU16(addr, space);
}

template <>
PowerPC::TryReadResult<u32> TryReadValueFromEmulatedMemory(u32 addr,
                                                           PowerPC::RequestedAddressSpace space)
{
  return PowerPC::HostTryReadU32(addr, space);
}

template <>
PowerPC::TryReadResult<u64> TryReadValueFromEmulatedMemory(u32 addr,
                                                           PowerPC::RequestedAddressSpace space)
{
  return PowerPC::HostTryReadU64(addr, space);
}

template <>
PowerPC::TryReadResult<s8> TryReadValueFromEmulatedMemory(u32 addr,
                                                          PowerPC::RequestedAddressSpace space)
{
  auto tmp = PowerPC::HostTryReadU8(addr, space);
  if (!tmp)
    return PowerPC::TryReadResult<s8>();
  return PowerPC::TryReadResult<s8>(tmp.translated, Common::BitCast<s8>(tmp.value));
}

template <>
PowerPC::TryReadResult<s16> TryReadValueFromEmulatedMemory(u32 addr,
                                                           PowerPC::RequestedAddressSpace space)
{
  auto tmp = PowerPC::HostTryReadU16(addr, space);
  if (!tmp)
    return PowerPC::TryReadResult<s16>();
  return PowerPC::TryReadResult<s16>(tmp.translated, Common::BitCast<s16>(tmp.value));
}

template <>
PowerPC::TryReadResult<s32> TryReadValueFromEmulatedMemory(u32 addr,
                                                           PowerPC::RequestedAddressSpace space)
{
  auto tmp = PowerPC::HostTryReadU32(addr, space);
  if (!tmp)
    return PowerPC::TryReadResult<s32>();
  return PowerPC::TryReadResult<s32>(tmp.translated, Common::BitCast<s32>(tmp.value));
}

template <>
PowerPC::TryReadResult<s64> TryReadValueFromEmulatedMemory(u32 addr,
                                                           PowerPC::RequestedAddressSpace space)
{
  auto tmp = PowerPC::HostTryReadU64(addr, space);
  if (!tmp)
    return PowerPC::TryReadResult<s64>();
  return PowerPC::TryReadResult<s64>(tmp.translated, Common::BitCast<s64>(tmp.value));
}

template <>
PowerPC::TryReadResult<float> TryReadValueFromEmulatedMemory(u32 addr,
                                                             PowerPC::RequestedAddressSpace space)
{
  return PowerPC::HostTryReadF32(addr, space);
}

template <>
PowerPC::TryReadResult<double> TryReadValueFromEmulatedMemory(u32 addr,
                                                              PowerPC::RequestedAddressSpace space)
{
  return PowerPC::HostTryReadF64(addr, space);
}
}  // namespace

template <typename T>
Common::Result<Cheats::SearchErrorCode, std::vector<Cheats::SearchResult<T>>>
Cheats::NewSearch(const std::vector<Cheats::MemoryRange>& memory_ranges,
                  PowerPC::RequestedAddressSpace address_space, bool aligned,
                  const std::function<bool(const T& value)>& validator)
{
  const u32 data_size = sizeof(T);
  std::vector<Cheats::SearchResult<T>> results;
  Cheats::SearchErrorCode error_code = Cheats::SearchErrorCode::Success;
  Core::RunAsCPUThread([&] {
    const Core::State core_state = Core::GetState();
    if (core_state != Core::State::Running && core_state != Core::State::Paused)
    {
      error_code = Cheats::SearchErrorCode::NoEmulationActive;
      return;
    }

    if (address_space == PowerPC::RequestedAddressSpace::Virtual && !MSR.DR)
    {
      error_code = Cheats::SearchErrorCode::VirtualAddressesCurrentlyNotAccessible;
      return;
    }

    for (const Cheats::MemoryRange& range : memory_ranges)
    {
      const u32 increment_per_loop = aligned ? data_size : 1;
      const u32 start_address = aligned ? Common::AlignUp(range.m_start, data_size) : range.m_start;
      const u64 aligned_length = range.m_length - (start_address - range.m_start);
      const u64 length = aligned_length - (data_size - 1);
      for (u64 i = 0; i < length; i += increment_per_loop)
      {
        const u32 addr = start_address + i;
        const auto current_value = TryReadValueFromEmulatedMemory<T>(addr, address_space);
        if (!current_value)
          continue;

        if (validator(current_value.value))
        {
          auto& r = results.emplace_back();
          r.m_value = current_value.value;
          r.m_value_state = current_value.translated ?
                                Cheats::SearchResultValueState::ValueFromVirtualMemory :
                                Cheats::SearchResultValueState::ValueFromPhysicalMemory;
          r.m_address = addr;
        }
      }
    }
  });
  if (error_code == Cheats::SearchErrorCode::Success)
    return results;
  return error_code;
}

template <typename T>
Common::Result<Cheats::SearchErrorCode, std::vector<Cheats::SearchResult<T>>>
Cheats::NextSearch(const std::vector<Cheats::SearchResult<T>>& previous_results,
                   PowerPC::RequestedAddressSpace address_space,
                   const std::function<bool(const T& new_value, const T& old_value)>& validator)
{
  std::vector<Cheats::SearchResult<T>> results;
  Cheats::SearchErrorCode error_code = Cheats::SearchErrorCode::Success;
  Core::RunAsCPUThread([&] {
    const Core::State core_state = Core::GetState();
    if (core_state != Core::State::Running && core_state != Core::State::Paused)
    {
      error_code = Cheats::SearchErrorCode::NoEmulationActive;
      return;
    }

    if (address_space == PowerPC::RequestedAddressSpace::Virtual && !MSR.DR)
    {
      error_code = Cheats::SearchErrorCode::VirtualAddressesCurrentlyNotAccessible;
      return;
    }

    for (const auto& previous_result : previous_results)
    {
      const u32 addr = previous_result.m_address;
      const auto current_value = TryReadValueFromEmulatedMemory<T>(addr, address_space);
      if (!current_value)
      {
        auto& r = results.emplace_back();
        r.m_address = addr;
        r.m_value_state = Cheats::SearchResultValueState::AddressNotAccessible;
        continue;
      }

      // if the previous state was invalid we always update the value to avoid getting stuck in an
      // invalid state
      if (!previous_result.IsValueValid() ||
          validator(current_value.value, previous_result.m_value))
      {
        auto& r = results.emplace_back();
        r.m_value = current_value.value;
        r.m_value_state = current_value.translated ?
                              Cheats::SearchResultValueState::ValueFromVirtualMemory :
                              Cheats::SearchResultValueState::ValueFromPhysicalMemory;
        r.m_address = addr;
      }
    }
  });
  if (error_code == Cheats::SearchErrorCode::Success)
    return results;
  return error_code;
}

Cheats::CheatSearchSessionBase::~CheatSearchSessionBase() = default;

template <typename T>
Cheats::CheatSearchSession<T>::CheatSearchSession(std::vector<MemoryRange> memory_ranges,
                                                  PowerPC::RequestedAddressSpace address_space,
                                                  bool aligned)
    : m_memory_ranges(std::move(memory_ranges)), m_address_space(address_space), m_aligned(aligned)
{
}

template <typename T>
Cheats::CheatSearchSession<T>::CheatSearchSession(const CheatSearchSession& session) = default;

template <typename T>
Cheats::CheatSearchSession<T>::CheatSearchSession(CheatSearchSession&& session) = default;

template <typename T>
Cheats::CheatSearchSession<T>&
Cheats::CheatSearchSession<T>::operator=(const CheatSearchSession& session) = default;

template <typename T>
Cheats::CheatSearchSession<T>&
Cheats::CheatSearchSession<T>::operator=(CheatSearchSession&& session) = default;

template <typename T>
Cheats::CheatSearchSession<T>::~CheatSearchSession() = default;

template <typename T>
void Cheats::CheatSearchSession<T>::SetCompareType(CompareType compare_type)
{
  m_compare_type = compare_type;
}

template <typename T>
void Cheats::CheatSearchSession<T>::SetFilterType(FilterType filter_type)
{
  m_filter_type = filter_type;
}

template <typename T>
static std::optional<T> ParseValue(const std::string& str)
{
  if (str.empty())
    return std::nullopt;

  T tmp;
  if (TryParse(str, &tmp))
    return tmp;

  return std::nullopt;
}

template <typename T>
bool Cheats::CheatSearchSession<T>::SetValueFromString(const std::string& value_as_string)
{
  m_value = ParseValue<T>(value_as_string);
  return m_value.has_value();
}

template <typename T>
void Cheats::CheatSearchSession<T>::ResetResults()
{
  m_first_search_done = false;
  m_search_results.clear();
}

template <typename T>
static std::function<bool(const T& new_value)>
MakeCompareFunctionForSpecificValue(Cheats::CompareType op, const T& old_value)
{
  switch (op)
  {
  case Cheats::CompareType::Equal:
    return [&](const T& new_value) { return new_value == old_value; };
  case Cheats::CompareType::NotEqual:
    return [&](const T& new_value) { return new_value != old_value; };
  case Cheats::CompareType::Less:
    return [&](const T& new_value) { return new_value < old_value; };
  case Cheats::CompareType::LessOrEqual:
    return [&](const T& new_value) { return new_value <= old_value; };
  case Cheats::CompareType::Greater:
    return [&](const T& new_value) { return new_value > old_value; };
  case Cheats::CompareType::GreaterOrEqual:
    return [&](const T& new_value) { return new_value >= old_value; };
  default:
    assert(0);
    return nullptr;
  }
}

template <typename T>
static std::function<bool(const T& old_value, const T& new_value)>
MakeCompareFunctionForLastValue(Cheats::CompareType op)
{
  switch (op)
  {
  case Cheats::CompareType::Equal:
    return [](const T& new_value, const T& old_value) { return new_value == old_value; };
  case Cheats::CompareType::NotEqual:
    return [](const T& new_value, const T& old_value) { return new_value != old_value; };
  case Cheats::CompareType::Less:
    return [](const T& new_value, const T& old_value) { return new_value < old_value; };
  case Cheats::CompareType::LessOrEqual:
    return [](const T& new_value, const T& old_value) { return new_value <= old_value; };
  case Cheats::CompareType::Greater:
    return [](const T& new_value, const T& old_value) { return new_value > old_value; };
  case Cheats::CompareType::GreaterOrEqual:
    return [](const T& new_value, const T& old_value) { return new_value >= old_value; };
  default:
    assert(0);
    return nullptr;
  }
}

template <typename T>
Cheats::SearchErrorCode Cheats::CheatSearchSession<T>::RunSearch()
{
  Common::Result<SearchErrorCode, std::vector<SearchResult<T>>> result =
      Cheats::SearchErrorCode::InvalidParameters;
  if (m_filter_type == FilterType::CompareAgainstSpecificValue)
  {
    if (!m_value)
      return Cheats::SearchErrorCode::InvalidParameters;

    auto func = MakeCompareFunctionForSpecificValue<T>(m_compare_type, *m_value);
    if (m_first_search_done)
    {
      result = Cheats::NextSearch<T>(
          m_search_results, m_address_space,
          [&func](const T& new_value, const T& old_value) { return func(new_value); });
    }
    else
    {
      result = Cheats::NewSearch<T>(m_memory_ranges, m_address_space, m_aligned, func);
    }
  }
  else if (m_filter_type == FilterType::CompareAgainstLastValue)
  {
    if (!m_first_search_done)
      return Cheats::SearchErrorCode::InvalidParameters;

    result = Cheats::NextSearch<T>(m_search_results, m_address_space,
                                   MakeCompareFunctionForLastValue<T>(m_compare_type));
  }
  else if (m_filter_type == FilterType::DoNotFilter)
  {
    if (m_first_search_done)
    {
      result = Cheats::NextSearch<T>(m_search_results, m_address_space,
                                     [](const T& v1, const T& v2) { return true; });
    }
    else
    {
      result = Cheats::NewSearch<T>(m_memory_ranges, m_address_space, m_aligned,
                                    [](const T& v) { return true; });
    }
  }

  if (result.Succeeded())
  {
    m_search_results = std::move(*result);
    m_first_search_done = true;
    return Cheats::SearchErrorCode::Success;
  }

  return result.Error();
}

template <typename T>
size_t Cheats::CheatSearchSession<T>::GetMemoryRangeCount()
{
  return m_memory_ranges.size();
}

template <typename T>
Cheats::MemoryRange Cheats::CheatSearchSession<T>::GetMemoryRange(size_t index)
{
  return m_memory_ranges[index];
}

template <typename T>
PowerPC::RequestedAddressSpace Cheats::CheatSearchSession<T>::GetAddressSpace()
{
  return m_address_space;
}

template <typename T>
Cheats::DataType Cheats::CheatSearchSession<T>::GetDataType()
{
  return Cheats::GetDataType(Cheats::SearchValue{T(0)});
}

template <typename T>
bool Cheats::CheatSearchSession<T>::GetAligned()
{
  return m_aligned;
}

template <typename T>
size_t Cheats::CheatSearchSession<T>::GetResultCount() const
{
  return m_search_results.size();
}

template <typename T>
size_t Cheats::CheatSearchSession<T>::GetValidValueCount() const
{
  const auto& results = m_search_results;
  size_t count = 0;
  for (const auto& r : results)
  {
    if (r.IsValueValid())
      ++count;
  }
  return count;
}

template <typename T>
u32 Cheats::CheatSearchSession<T>::GetResultAddress(size_t index) const
{
  return m_search_results[index].m_address;
}

template <typename T>
T Cheats::CheatSearchSession<T>::GetResultValue(size_t index) const
{
  return m_search_results[index].m_value;
}

template <typename T>
Cheats::SearchValue Cheats::CheatSearchSession<T>::GetResultValueAsSearchValue(size_t index) const
{
  return Cheats::SearchValue{m_search_results[index].m_value};
}

template <typename T>
std::string Cheats::CheatSearchSession<T>::GetResultValueAsString(size_t index, bool hex) const
{
  if (GetResultValueState(index) == Cheats::SearchResultValueState::AddressNotAccessible)
    return "(inaccessible)";

  if (hex)
  {
    if constexpr (std::is_same_v<T, float>)
      return fmt::format("0x{0:08x}", Common::BitCast<u32>(m_search_results[index].m_value));
    else if constexpr (std::is_same_v<T, double>)
      return fmt::format("0x{0:016x}", Common::BitCast<u64>(m_search_results[index].m_value));
    else
      return fmt::format("0x{0:0{1}x}", m_search_results[index].m_value, sizeof(T) * 2);
  }

  return fmt::format("{}", m_search_results[index].m_value);
}

template <typename T>
Cheats::SearchResultValueState
Cheats::CheatSearchSession<T>::GetResultValueState(size_t index) const
{
  return m_search_results[index].m_value_state;
}

template <typename T>
bool Cheats::CheatSearchSession<T>::WasFirstSearchDone() const
{
  return m_first_search_done;
}

template <typename T>
std::unique_ptr<Cheats::CheatSearchSessionBase> Cheats::CheatSearchSession<T>::Clone() const
{
  return std::make_unique<Cheats::CheatSearchSession<T>>(*this);
}

template <typename T>
std::unique_ptr<Cheats::CheatSearchSessionBase>
Cheats::CheatSearchSession<T>::ClonePartial(const std::vector<size_t>& result_indices) const
{
  const auto& results = m_search_results;
  std::vector<SearchResult<T>> partial_results;
  partial_results.reserve(result_indices.size());
  for (size_t idx : result_indices)
    partial_results.push_back(results[idx]);

  auto c =
      std::make_unique<Cheats::CheatSearchSession<T>>(m_memory_ranges, m_address_space, m_aligned);
  c->m_search_results = std::move(partial_results);
  c->m_compare_type = this->m_compare_type;
  c->m_filter_type = this->m_filter_type;
  c->m_value = this->m_value;
  c->m_first_search_done = this->m_first_search_done;
  return c;
}

template class Cheats::CheatSearchSession<u8>;
template class Cheats::CheatSearchSession<u16>;
template class Cheats::CheatSearchSession<u32>;
template class Cheats::CheatSearchSession<u64>;
template class Cheats::CheatSearchSession<s8>;
template class Cheats::CheatSearchSession<s16>;
template class Cheats::CheatSearchSession<s32>;
template class Cheats::CheatSearchSession<s64>;
template class Cheats::CheatSearchSession<float>;
template class Cheats::CheatSearchSession<double>;

std::unique_ptr<Cheats::CheatSearchSessionBase>
Cheats::MakeSession(std::vector<MemoryRange> memory_ranges,
                    PowerPC::RequestedAddressSpace address_space, bool aligned, DataType data_type)
{
  switch (data_type)
  {
  case Cheats::DataType::U8:
    return std::make_unique<CheatSearchSession<u8>>(std::move(memory_ranges), address_space,
                                                    aligned);
  case Cheats::DataType::U16:
    return std::make_unique<CheatSearchSession<u16>>(std::move(memory_ranges), address_space,
                                                     aligned);
  case Cheats::DataType::U32:
    return std::make_unique<CheatSearchSession<u32>>(std::move(memory_ranges), address_space,
                                                     aligned);
  case Cheats::DataType::U64:
    return std::make_unique<CheatSearchSession<u64>>(std::move(memory_ranges), address_space,
                                                     aligned);
  case Cheats::DataType::S8:
    return std::make_unique<CheatSearchSession<s8>>(std::move(memory_ranges), address_space,
                                                    aligned);
  case Cheats::DataType::S16:
    return std::make_unique<CheatSearchSession<s16>>(std::move(memory_ranges), address_space,
                                                     aligned);
  case Cheats::DataType::S32:
    return std::make_unique<CheatSearchSession<s32>>(std::move(memory_ranges), address_space,
                                                     aligned);
  case Cheats::DataType::S64:
    return std::make_unique<CheatSearchSession<s64>>(std::move(memory_ranges), address_space,
                                                     aligned);
  case Cheats::DataType::F32:
    return std::make_unique<CheatSearchSession<float>>(std::move(memory_ranges), address_space,
                                                       aligned);
  case Cheats::DataType::F64:
    return std::make_unique<CheatSearchSession<double>>(std::move(memory_ranges), address_space,
                                                        aligned);
  default:
    assert(0);
    return nullptr;
  }
}
