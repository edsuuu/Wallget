#pragma once

#include "common/xaml.h"

void ApplyStartLogo(FrameworkElement const& icon);
void RefreshStartLogo();
void ApplyBlurBackground(FrameworkElement const& background);
void RefreshBlurBackground();
void HookTaskButtons(FrameworkElement const& list);
void MirrorRunningIndicator(FrameworkElement const& original);
void ApplySearchGlass(Controls::Border const& box);
void ApplySearchWidth(FrameworkElement const& grid);
void ApplyOverflowGlass(Controls::Border const& border);
void AddMediaPlayer(FrameworkElement const& frame);
void RefreshMediaPlayer();
void AddSystemStats(FrameworkElement const& frame);
void RefreshSystemStats();
void AddTaskbarIsland(winrt::Windows::Foundation::IInspectable const& object);
