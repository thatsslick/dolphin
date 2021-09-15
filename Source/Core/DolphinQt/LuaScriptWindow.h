// Copyright 2021 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include <QDialog>

#include "Common/CommonTypes.h"
#include "Core/LUA/Lua.h"

class QAction;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QMenu;
class QPixmap;
class QPushButton;
class QString;
class QTableWidget;
class QTextEdit;
class QTimer;
class QToolButton;

class LuaScriptWindow : public QDialog
{
  Q_OBJECT
public:
  explicit LuaScriptWindow(QWidget* parent = nullptr);
  ~LuaScriptWindow();

private:
  void CreateWidgets();
  void ConnectWidgets();
  void ExecuteScript();
  void CancelScript();
  QStringList GetScriptList();

  // Actions
  QGroupBox* m_script_box;
  QPushButton* m_execute_button;
  QPushButton* m_cancel_button;
  QComboBox* m_script_dropdown;
};
