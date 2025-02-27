// Copyright 2021 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/LuaScriptWindow.h"

#include <algorithm>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPixmap>
#include <QPushButton>
#include <QString>
#include <QStringList>
#include <QTableWidget>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>

#include "Common/Assert.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Common/VariantUtil.h"

#include "DolphinQt/QtUtils/ModalMessageBox.h"

#include "Core/LUA/Lua.h"

LuaScriptWindow::LuaScriptWindow(QWidget* parent) : QDialog(parent)
{
  CreateWidgets();
  ConnectWidgets();

  // Set window dimensions
  resize(200, 100);

  setWindowTitle(tr("Execute Lua Script"));
  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
}

LuaScriptWindow::~LuaScriptWindow() = default;

void LuaScriptWindow::CreateWidgets()
{
  // Actions
  m_script_box = new QGroupBox(tr("Script File"));

  m_execute_button = new QPushButton(tr("Start"));
  m_cancel_button = new QPushButton(tr("Cancel"));

  m_script_dropdown = new QComboBox();
  m_script_dropdown->setSizePolicy(QSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed));
  QStringList scripts = GetScriptList();
  m_script_dropdown->addItems(scripts);


  auto* layout = new QGridLayout;
  auto* secondlayout = new QGridLayout;

  m_script_box->setLayout(secondlayout);
  secondlayout->addWidget(m_script_dropdown, 0, 0, 1, 2);

  layout->addWidget(m_script_box, 0, 0, 1, 2);
  layout->addWidget(m_execute_button, 1, 0);
  layout->addWidget(m_cancel_button, 1, 1);
  layout->setColumnMinimumWidth(0, 100);
  layout->setColumnMinimumWidth(1, 100);

  setLayout(layout);
}

void LuaScriptWindow::ConnectWidgets()
{
  connect(m_execute_button, &QPushButton::clicked, this, &LuaScriptWindow::ExecuteScript);
  connect(m_cancel_button, &QPushButton::clicked, this, &LuaScriptWindow::CancelScript);
}

void LuaScriptWindow::ExecuteScript()
{
  ModalMessageBox::warning(
      this, tr("Woah!! Cool."),
      tr("If you see this, then I somehow got the GUI set up.\nThis is an example of how we would pass in "));
      Lua::LoadScript(m_script_dropdown->currentText().toStdString());
}

void LuaScriptWindow::CancelScript()
{
      Lua::TerminateScript(m_script_dropdown->currentText().toStdString());
}

QStringList LuaScriptWindow::GetScriptList()
{
  QStringList scripts;
  // Look in /Sys/Scripts for script files
  std::string scriptsFolder = File::GetSysDirectory() + "Scripts";
  if (!File::Exists(scriptsFolder))
  {
    return scripts;
  }

  File::FSTEntry fstentry = File::ScanDirectoryTree(scriptsFolder, false);

  if (!fstentry.isDirectory)
  {
    return scripts;
  }

  for (int i = 0; i < fstentry.size; i++)
  {
    File::FSTEntry child = fstentry.children[i];

    if (child.isDirectory)
    {
      continue;
    }

    const QString filename = QString::fromStdString(child.virtualName);

    // Assert that this is a .lua file
    QString suffix = QFileInfo(filename).suffix();

    if (suffix != tr("lua"))
    {
      continue;
    }

    // Ignore scripts with "_" prefix, as these are autorun
    if (filename.left(1) == tr("_"))
    {
      continue;
    }

    scripts.append(filename);
  }

  return scripts;
}
