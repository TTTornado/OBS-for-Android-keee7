#include "ScreenAudioStartTransaction.hpp"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

static void expect(const std::vector<std::string> &actual, const std::vector<std::string> &expected)
{
	if (actual == expected)
		return;
	std::fprintf(stderr, "rollback order mismatch; actual:");
	for (const std::string &call : actual)
		std::fprintf(stderr, " %s", call.c_str());
	std::fprintf(stderr, "; expected:");
	for (const std::string &call : expected)
		std::fprintf(stderr, " %s", call.c_str());
	std::fprintf(stderr, "\n");
	std::abort();
}

int main()
{
	std::vector<std::string> calls;
	auto clear = [&] { calls.push_back("clear"); };
	auto stop = [&] { calls.push_back("stop"); };
	auto release = [&] { calls.push_back("release"); };

	{
		ScreenAudioStartTransaction txn(clear, stop, release);
		txn.arm();
		assert(txn.rollback());
		expect(calls, {"clear", "stop"});
		assert(!txn.rollback()); // 同一事务只回滚一次
	}

	calls.clear();
	{
		ScreenAudioStartTransaction txn(clear, stop, release);
		txn.arm();
		txn.markScratchCreated();
		txn.commit();
		// 成功提交后析构不得再回滚
	}
	assert(calls.empty());

	calls.clear();
	{
		ScreenAudioStartTransaction txn(clear, stop, release);
		txn.arm();
		// 析构时自动回滚，且没有 scratch 时不能调用 release
	}
	expect(calls, {"clear", "stop"});

	calls.clear();
	{
		ScreenAudioStartTransaction txn(clear, stop, release);
		txn.arm();
		txn.markScratchCreated();
		assert(txn.rollback());
		expect(calls, {"clear", "stop", "release"});
	}

	return 0;
}
