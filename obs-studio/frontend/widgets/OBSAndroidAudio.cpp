/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "OBSAndroidAudio.hpp"

#include <obs.h>
#include <obs-android.h>

#include <utility/android-jni.hpp>

#include <QStringList>

/* Java side (frontend/cmake/android/java/com/obsproject/studio/ObsAudioHost.java)
 * exposes only public static methods. */
static constexpr auto hostClass = "com/obsproject/studio/ObsAudioHost";

/* Wire format from snapshot(): id|type|isSource|product|label, entries split by ';'. */
static int getAudioDevices(void *param, struct obs_android_audio_device *out, int max)
{
	(void)param;
	const QJniObject snap =
		QJniObject::callStaticObjectMethod(hostClass, "snapshot", "()Ljava/lang/String;");
	if (!snap.isValid())
		return -1; /* bridge missing, not "no devices" */

	const QString joined = snap.toString();
	if (joined.isEmpty())
		return 0;

	const QStringList entries = joined.split(QLatin1Char(';'), Qt::SkipEmptyParts);
	int n = 0;
	int dropped = 0;
	for (const QString &e : entries) {
		if (n >= max)
			break;
		const QStringList f = e.split(QLatin1Char('|'));
		if (f.size() < 5) {
			dropped++;
			continue;
		}
		bool okId = false, okType = false;
		const int id = f.at(0).toInt(&okId);
		const int type = f.at(1).toInt(&okType);
		if (!okId || !okType) {
			dropped++;
			continue;
		}
		out[n].id = id;
		out[n].type = type;
		out[n].is_source = (f.at(2) == QLatin1String("1"));
		AndroidJni::copyUtf8(out[n].product, sizeof(out[n].product), f.at(3));
		AndroidJni::copyUtf8(out[n].label, sizeof(out[n].label), f.at(4));
		n++;
	}
	if (n == 0 && dropped > 0)
		return -1; /* protocol mismatch, not an empty list */
	return n;
}

static bool hasCapturePermission(void *param)
{
	(void)param;
	return QJniObject::callStaticMethod<jboolean>(hostClass, "hasRecordPermission", "()Z") != 0;
}

static struct obs_android_audio_host g_audioHost = {
	nullptr,
	getAudioDevices,
	hasCapturePermission,
};

bool OBSAndroidAudioInit()
{
	const QJniObject ctx = AndroidJni::context();
	if (!ctx.isValid()) {
		blog(LOG_WARNING, "Android audio host: no Context from QtNative");
		return false;
	}
	const jboolean ok = QJniObject::callStaticMethod<jboolean>(hostClass, "install",
								   "(Landroid/content/Context;)Z",
								   ctx.object<jobject>());
	if (!ok) {
		blog(LOG_WARNING, "Android audio host: ObsAudioHost.install() failed");
		return false;
	}
	obs_android_set_audio_host(&g_audioHost);
	blog(LOG_INFO, "Android audio host registered (AAudio input device list)");
	return true;
}

void OBSAndroidAudioShutdown()
{
	obs_android_set_audio_host(nullptr);
}
