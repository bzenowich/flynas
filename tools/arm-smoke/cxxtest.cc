// C++ runtime check for run-multiuser.sh: libcxxrt (exceptions, RTTI),
// libc++ (regex, charconv, filesystem, locale, iostreams) and atomic
// wait/notify across threads (umtx_sleep/umtx_wakeup).  Built dynamic and
// static; prints "CXX ok".
#include <atomic>
#include <charconv>
#include <filesystem>
#include <iostream>
#include <locale>
#include <map>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static int fails;
#define CHECK(c) do { if (!(c)) { std::cout << "FAIL " #c "\n"; fails++; } } while (0)

struct Base { virtual ~Base() = default; };
struct Derived : Base { int x = 7; };

int main() {
	try {
		throw std::runtime_error("boom");
	} catch (const std::exception& e) {
		CHECK(std::string(e.what()) == "boom");
	}
	Base* b = new Derived;
	CHECK(dynamic_cast<Derived*>(b) && dynamic_cast<Derived*>(b)->x == 7);
	delete b;

	std::regex re("(\\w+)@(\\w+)\\.org");
	std::smatch m;
	std::string s = "mail root@dragonfly.org now";
	CHECK(std::regex_search(s, m, re) && m[2] == "dragonfly");

	double d = 0;
	const char* f = "3.25e2";
	auto r = std::from_chars(f, f + 6, d);
	CHECK(r.ec == std::errc() && d == 325.0);
	char buf[32];
	auto t = std::to_chars(buf, buf + sizeof buf, 0.1);
	CHECK(std::string(buf, t.ptr) == "0.1");

	CHECK(std::filesystem::exists("/etc/passwd"));
	CHECK(std::filesystem::is_directory("/usr/lib"));

	std::map<std::string, int> mp{{"a", 1}, {"b", 2}};
	std::ostringstream os;
	for (auto& [k, v] : mp) os << k << v;
	CHECK(os.str() == "a1b2");

	CHECK(std::isalpha('x', std::locale::classic()) && !std::isdigit('x', std::locale::classic()));
	CHECK(std::toupper('q', std::locale::classic()) == 'Q');

	// atomic wait/notify across threads (umtx_sleep/umtx_wakeup)
	std::atomic<int> flag{0};
	std::vector<std::thread> th;
	std::atomic<int> woke{0};
	for (int i = 0; i < 4; i++)
		th.emplace_back([&] { flag.wait(0); woke++; });
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	flag = 1;
	flag.notify_all();
	for (auto& x : th) x.join();
	CHECK(woke == 4);

	std::cout << (fails ? "CXX fail" : "CXX ok") << std::endl;
	return fails != 0;
}
