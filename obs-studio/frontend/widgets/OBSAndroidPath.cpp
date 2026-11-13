#include "OBSAndroidPath.hpp"

#include "OBSAndroidDisplay.hpp" // obsAndroidDisplayLog：与 3-3/3-4 其余探针同一个 tag，一条 logcat 看全

#include <qt-wrappers.hpp> // OBSMessageBox

#include <utility/android-jni.hpp>

#include <QApplication>
#include <QClipboard>
#include <QJniObject>
#include <obs-android.h>

static constexpr auto storageClass = "com/obsproject/studio/ObsStorageHost";

static int openDocument(const char *path, const char *mode)
{
	auto context = AndroidJni::context();
	if (!context.isValid())
		return -1;
	auto uri = QJniObject::fromString(QString::fromUtf8(path));
	auto access = QJniObject::fromString(QString::fromUtf8(mode));
	return QJniObject::callStaticMethod<jint>(storageClass, "open",
		"(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)I",
		context.object<jobject>(), uri.object<jstring>(), access.object<jstring>());
}

void obsAndroidStorageInit()
{
	obs_android_set_storage_open(openDocument);
}

bool obsAndroidStorageManaged(const QString &path)
{
	auto p = QJniObject::fromString(path);
	return QJniObject::callStaticMethod<jboolean>(storageClass, "managed", "(Ljava/lang/String;)Z",
						    p.object<jstring>());
}

bool obsAndroidStorageWritable(const QString &path)
{
	auto context = AndroidJni::context();
	if (!context.isValid())
		return false;
	auto p = QJniObject::fromString(path);
	return QJniObject::callStaticMethod<jboolean>(storageClass, "writable",
		"(Landroid/content/Context;Ljava/lang/String;)Z", context.object<jobject>(), p.object<jstring>());
}

QString obsAndroidStorageCreate(const QString &path, const QString &name)
{
	auto context = AndroidJni::context();
	if (!context.isValid())
		return {};
	auto p = QJniObject::fromString(path);
	auto n = QJniObject::fromString(name);
	return QJniObject::callStaticObjectMethod(storageClass, "create",
		"(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
		context.object<jobject>(), p.object<jstring>(), n.object<jstring>()).toString();
}

qint64 obsAndroidStorageSpace(const QString &path)
{
	auto context = AndroidJni::context();
	if (!context.isValid())
		return -1;
	auto p = QJniObject::fromString(path);
	return QJniObject::callStaticMethod<jlong>(storageClass, "space",
		"(Landroid/content/Context;Ljava/lang/String;)J", context.object<jobject>(), p.object<jstring>());
}

void obsAndroidShowLocalPath(QWidget *parent, const QString &title, const QString &path)
{
	QClipboard *clipboard = QApplication::clipboard();
	clipboard->setText(path);

	// 回读自证：Android 上剪贴板写入可能被平台规则挡掉，所以"已复制"这句话只在回读一致时才说。
	const bool copied = (clipboard->text(QClipboard::Clipboard) == path);
	obsAndroidDisplayLog("3-4 openUrl 替代：%s → %s | 剪贴板回读=%s", title.toUtf8().constData(),
			     path.toUtf8().constData(), copied ? "一致" : "不一致");

	OBSMessageBox::information(parent, title,
				   QString("%1\n\n请使用手机的文件管理器查看此位置。%2")
					   .arg(path, copied ? "\n\n路径已复制到剪贴板，可粘贴到其他应用。" : ""));
}
