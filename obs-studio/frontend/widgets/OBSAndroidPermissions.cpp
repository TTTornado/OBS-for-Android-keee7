#include "OBSAndroidPermissions.hpp"

#ifdef __ANDROID__

#include <utility/android-jni.hpp>

#include <QJniObject>

#include <obs.hpp>

/* Java 侧宿主（frontend/cmake/android/java/com/obsproject/studio/ObsPermissionHost.java） */
static const char kPermissionClass[] = "com/obsproject/studio/ObsPermissionHost";

/* requestPermissions 只能在 Activity 上调（Context 没有这个方法），所以这里要的是 Activity 而不是
 * 通用 Context —— 与 ObsForegroundService 那边只要 Context 不同。 */
static QString callDescribe(const QJniObject &ctx)
{
	const QJniObject s = QJniObject::callStaticObjectMethod(kPermissionClass, "describe",
								"(Landroid/content/Context;)Ljava/lang/String;",
								ctx.object<jobject>());
	return s.isValid() ? s.toString() : QStringLiteral("describe 调不到（类没进 APK？）");
}

bool obsAndroidPermissionsRequestStartup(QString *detail)
{
	const QJniObject act = AndroidJni::activity();
	if (!act.isValid()) {
		const QString line = QStringLiteral("权限申请没起来：拿不到 QtNative.activity()");
		if (detail)
			*detail = line;
		blog(LOG_WARNING, "OBSAndroidPermissions[Android]: %s", line.toUtf8().constData());
		return false;
	}

	const jboolean posted = QJniObject::callStaticMethod<jboolean>(kPermissionClass, "requestStartup",
								       "(Landroid/app/Activity;)Z",
								       act.object<jobject>()) != 0;
	/* describe 里的"待申请/动作"才是这次到底弹了框还是本来就全有的判据；posted 只表示没拿到 Activity。 */
	const QString line = QStringLiteral("requestStartup=%1 %2")
				     .arg(posted ? QStringLiteral("true") : QStringLiteral("false"))
				     .arg(callDescribe(act));
	if (detail)
		*detail = line;
	blog(posted ? LOG_INFO : LOG_WARNING, "OBSAndroidPermissions[Android]: %s", line.toUtf8().constData());
	return posted;
}

QString obsAndroidPermissionsStatus()
{
	return callDescribe(AndroidJni::activity());
}


#endif // __ANDROID__
