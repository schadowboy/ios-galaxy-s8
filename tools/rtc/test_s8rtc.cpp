// Host check of the pure epoch <-> RTC register conversions in S8Rtc.cpp (included as is, kernel part compiled out).
#define ARM64_BOARD_CONFIG_A5XELTE 1
#define S8RTC_HOST_TEST 1
#include "S8Rtc.cpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void one(uint64_t e)
{
	uint8_t r[S8RTC_NREGS];
	int64_t back;
	time_t t = (time_t)e;
	struct tm tm;

	gmtime_r(&t, &tm);
	CHECK(s8rtc_epoch_to_regs(e, r) == 0, "epoch_to_regs %llu", (unsigned long long)e);
	CHECK(r[0] == tm.tm_sec && r[1] == tm.tm_min, "sec/min %llu", (unsigned long long)e);
	CHECK((r[2] & 0x1f) == tm.tm_hour && ((r[2] & 0x40) != 0) == (tm.tm_hour >= 12), "hour %llu: %02x vs %d", (unsigned long long)e, r[2], tm.tm_hour);
	CHECK(r[3] == (1u << tm.tm_wday), "weekday %llu: %02x vs %d", (unsigned long long)e, r[3], tm.tm_wday);
	CHECK(r[4] == tm.tm_mday && r[5] == tm.tm_mon + 1 && r[6] == tm.tm_year - 100, "date %llu", (unsigned long long)e);
	CHECK(s8rtc_regs_to_epoch(r, 2000, &back) == 0 && (uint64_t)back == e, "roundtrip %llu -> %lld", (unsigned long long)e, (long long)back);
}

int main(void)
{
	// every 977th second of 2000..2099 (prime step walks through all hours/minutes/weekdays), plus every day boundary
	for (uint64_t e = 946684800ULL; e <= 4102444799ULL; e += 977) {
		one(e);
	}
	for (uint64_t d = 10957; d <= 47481; d++) {
		one(d * 86400 - 1 > 946684800ULL ? d * 86400 - 1 : 946684800ULL);
		one(d * 86400);
		one(d * 86400 + 43200);
	}
	one(946684800ULL);       // 2000-01-01 00:00:00 (the minimum)
	one(4102444799ULL);      // 2099-12-31 23:59:59 (the maximum)
	one(1709164800ULL);      // 2024-02-29 00:00:00 leap day
	one(951782400ULL);       // 2000-02-29 (divisible by 400)
	one(1767225599ULL);      // 2025-12-31 23:59:59
	for (int i = 0; i < 200000; i++) {
		one(946684800ULL + ((uint64_t)rand() * 32768 + rand()) % (4102444799ULL - 946684800ULL + 1));
	}

	uint8_t r[S8RTC_NREGS];
	int64_t t;
	CHECK(s8rtc_epoch_to_regs(946684799ULL, r) == -1, "1999 accepted");
	CHECK(s8rtc_epoch_to_regs(4102444800ULL, r) == -1, "2100 accepted");
	CHECK(s8rtc_epoch_to_regs(0, r) == -1, "epoch 0 accepted");

	// the distinctive phone test time: 1947589617 = 2031-09-19 13:06:57 UTC, a Friday
	CHECK(s8rtc_epoch_to_regs(1947589617ULL, r) == 0, "distinctive");
	{
		time_t tt = (time_t)1947589617ULL;
		struct tm tm;
		gmtime_r(&tt, &tm);
		printf("1947589617 = %04d-%02d-%02d %02d:%02d:%02d wday %d -> regs %02x %02x %02x %02x %02x %02x %02x\n", tm.tm_year + 1900,
		    tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, tm.tm_wday, r[0], r[1], r[2], r[3], r[4], r[5], r[6]);
	}

	// decoder rejections
	uint8_t g[S8RTC_NREGS] = {57, 46, 0x40 | 13, 0x40, 17, 5, 31};
	CHECK(s8rtc_regs_to_epoch(g, 2024, &t) == 0, "valid image rejected");
	uint8_t b;
	b = g[0]; g[0] = 60; CHECK(s8rtc_regs_to_epoch(g, 2000, &t) == -1, "sec 60"); g[0] = b;
	b = g[1]; g[1] = 60; CHECK(s8rtc_regs_to_epoch(g, 2000, &t) == -1, "min 60"); g[1] = b;
	b = g[2]; g[2] = 24; CHECK(s8rtc_regs_to_epoch(g, 2000, &t) == -1, "hour 24"); g[2] = b;
	b = g[4]; g[4] = 0; CHECK(s8rtc_regs_to_epoch(g, 2000, &t) == -1, "day 0"); g[4] = b;
	b = g[5]; g[5] = 13; CHECK(s8rtc_regs_to_epoch(g, 2000, &t) == -1, "month 13"); g[5] = b;
	b = g[5]; g[5] = 0; CHECK(s8rtc_regs_to_epoch(g, 2000, &t) == -1, "month 0"); g[5] = b;
	b = g[6]; g[6] = 0; CHECK(s8rtc_regs_to_epoch(g, 2024, &t) == -1, "reset RTC year 2000 below min 2024"); g[6] = b;
	b = g[6]; g[6] = 100; CHECK(s8rtc_regs_to_epoch(g, 2000, &t) == -1, "year 2100"); g[6] = b;

	// the loader's formula (pmic.c / loader.c rtc_bootarg) on the same bytes
	{
		uint64_t raw = 0;
		uint8_t img[8] = {0x12 /* msec */, 57, 46, 13, 0x40, 17, 5, 31};
		for (int i = 0; i < 8; i++) raw |= (uint64_t)img[i] << (8 * i);
		unsigned sec = (raw >> 8) & 0x7f, min = (raw >> 16) & 0x7f, hour = (raw >> 24) & 0x1f, day = (raw >> 40) & 0x1f;
		unsigned mon = (raw >> 48) & 0x0f, year = 2000 + ((raw >> 56) & 0x7f);
		int64_t lt = s8rtc_days_from_civil(year, mon, day) * 86400 + hour * 3600 + min * 60 + sec;
		uint8_t rr[S8RTC_NREGS];
		int64_t mine;
		memcpy(rr, img + 1, 7);
		rr[2] = 13; /* the loader ignores the PM flag too */
		CHECK(s8rtc_regs_to_epoch(rr, 2000, &mine) == 0 && mine == lt, "loader formula %lld vs %lld", (long long)lt, (long long)mine);
	}

	printf(fails ? "%d FAILURES\n" : "all s8rtc pure-function checks passed\n", fails);
	return fails != 0;
}
