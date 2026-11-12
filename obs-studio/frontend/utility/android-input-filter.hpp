#pragma once

#ifdef __ANDROID__

class QObject;
class QWidget;

/* 安装 Android 应用级事件过滤器（长按=右键、BACK→Escape、顶层窗 clamp/flick）。 */
QObject *androidCreateInputFilter(QObject *parent, QWidget *(*mainWindow)());

#endif // __ANDROID__
