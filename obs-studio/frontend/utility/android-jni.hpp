#pragma once

/* Small, shared JNI helpers for the Android frontend.
 *
 * Keep this file dependency-light and header-only: it is included from the
 * frontend widget translation units, which are only compiled into the Android target.
 * The helpers deliberately do not cache Activity/Context objects: Activity
 * can be recreated and Application Context is not a drop-in replacement for
 * every Java API. Callers fetch the object they need at the point of use. */

#include <QByteArray>
#include <QJniEnvironment>
#include <QJniObject>
#include <QString>

#include <cstddef>
#include <cstring>
#include <jni.h>

namespace AndroidJni {

inline QJniObject activity()
{
	return QJniObject::callStaticObjectMethod("org/qtproject/qt/android/QtNative", "activity",
						  "()Landroid/app/Activity;");
}

inline QJniObject context()
{
	QJniObject ctx = QJniObject::callStaticObjectMethod(
		"org/qtproject/qt/android/QtNative", "getContext", "()Landroid/content/Context;");
	if (!ctx.isValid())
		ctx = activity();
	return ctx;
}

inline QString toString(const QJniObject &value)
{
	return value.isValid() ? value.toString() : QString();
}

/* Return the JNIEnv only when Qt successfully attached the current thread.
 * Callers must treat nullptr as an ordinary failure (return false/-1/0), never
 * dereference an invalid QJniEnvironment. */
inline JNIEnv *env(QJniEnvironment &environment)
{
	return environment.isValid() ? environment.jniEnv() : nullptr;
}

inline void copyUtf8(char *dst, size_t dstLen, const QString &src)
{
	if (!dst || dstLen == 0)
		return;
	const QByteArray utf8 = src.toUtf8();
	const size_t cap = dstLen - 1;
	const size_t n = static_cast<size_t>(utf8.size()) < cap ? static_cast<size_t>(utf8.size()) : cap;
	memcpy(dst, utf8.constData(), n);
	dst[n] = '\0';
}

} // namespace AndroidJni
