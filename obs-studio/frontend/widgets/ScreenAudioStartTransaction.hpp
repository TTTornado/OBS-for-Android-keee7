#pragma once

#include <functional>
#include <utility>

/* screenStartAudio 的小事务边界：Java 侧 startAudio() 成功之后，任何 native 失败都必须
 * 回滚 Java 的 AudioRecord；这个类只负责顺序，不依赖 Qt/JNI，故可在 host 上做故障注入测试。
 *
 * 回滚顺序固定为：
 *   1. 清除 raw JNI 调用留下的 pending exception（否则后面的 Java 调用会被异常挡住）
 *   2. 调 Java stopAudio()
 *   3. 如果本次调用创建了 scratch，再释放它
 */
class ScreenAudioStartTransaction {
public:
	using Action = std::function<void()>;

	ScreenAudioStartTransaction(Action clearException, Action stopAudio, Action releaseScratch)
		: clearException_(std::move(clearException)),
		  stopAudio_(std::move(stopAudio)),
		  releaseScratch_(std::move(releaseScratch))
	{
	}

	ScreenAudioStartTransaction(const ScreenAudioStartTransaction &) = delete;
	ScreenAudioStartTransaction &operator=(const ScreenAudioStartTransaction &) = delete;

	~ScreenAudioStartTransaction()
	{
		if (armed_)
			rollback();
	}

	void arm() { armed_ = true; }

	void markScratchCreated() { scratchCreated_ = true; }

	void commit() { armed_ = false; }

	bool rollback()
	{
		if (!armed_)
			return false;
		armed_ = false;
		if (clearException_)
			clearException_();
		if (stopAudio_)
			stopAudio_();
		if (scratchCreated_ && releaseScratch_)
			releaseScratch_();
		return true;
	}

private:
	Action clearException_;
	Action stopAudio_;
	Action releaseScratch_;
	bool armed_ = false;
	bool scratchCreated_ = false;
};
