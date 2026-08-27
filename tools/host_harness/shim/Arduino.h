// Host shim for <Arduino.h>.
// Part of tools/host_harness. NOT used by the firmware build.
//
// Provides just enough of the Arduino core API for the platform-agnostic
// MicroPatterns renderer sources to compile and run natively on macOS/Linux.
// Everything here is deliberately minimal: shim only what the core actually
// touches, so the harness does not perturb what it measures.
#ifndef HOST_SHIM_ARDUINO_H
#define HOST_SHIM_ARDUINO_H

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <climits>
#include <string>
#include <algorithm>

// ---------------------------------------------------------------------------
// String
// ---------------------------------------------------------------------------
// Arduino's String is a value type with copy semantics, implicit construction
// from numbers/chars, and the handful of methods used by the parser/runtime.
// We back it with std::string. See README.md "Shim fidelity" for the ways this
// differs from the real WString.
class String {
public:
    String() {}
    String(const char* s) : _s(s ? s : "") {}
    String(const char* s, unsigned int len) : _s(s ? s : "", s ? len : 0) {}
    String(const std::string& s) : _s(s) {}
    String(char c) : _s(1, c) {}
    explicit String(unsigned char v) { char b[16]; snprintf(b, sizeof(b), "%u", (unsigned)v); _s = b; }
    String(int v) { char b[24]; snprintf(b, sizeof(b), "%d", v); _s = b; }
    String(unsigned int v) { char b[24]; snprintf(b, sizeof(b), "%u", v); _s = b; }
    String(long v) { char b[32]; snprintf(b, sizeof(b), "%ld", v); _s = b; }
    String(unsigned long v) { char b[32]; snprintf(b, sizeof(b), "%lu", v); _s = b; }
    String(float v, unsigned char dec = 2) { char b[48]; snprintf(b, sizeof(b), "%.*f", (int)dec, (double)v); _s = b; }
    String(double v, unsigned char dec = 2) { char b[64]; snprintf(b, sizeof(b), "%.*f", (int)dec, v); _s = b; }

    // --- capacity / access -------------------------------------------------
    unsigned int length() const { return (unsigned int)_s.size(); }
    bool isEmpty() const { return _s.empty(); }
    void reserve(unsigned int n) { _s.reserve(n); }
    const char* c_str() const { return _s.c_str(); }
    // Arduino returns '\0' (not UB) for an out-of-range index.
    char operator[](unsigned int i) const { return i < _s.size() ? _s[i] : '\0'; }
    char operator[](int i) const { return (i >= 0 && (size_t)i < _s.size()) ? _s[i] : '\0'; }
    char charAt(unsigned int i) const { return (*this)[i]; }
    // ESP32 WString.h exposes raw begin()/end(), which the parser range-for's over.
    char* begin() { return &_s[0]; }
    char* end() { return &_s[0] + _s.size(); }
    const char* begin() const { return _s.data(); }
    const char* end() const { return _s.data() + _s.size(); }
    void setCharAt(unsigned int i, char c) { if (i < _s.size()) _s[i] = c; }

    // --- comparison --------------------------------------------------------
    bool equals(const String& o) const { return _s == o._s; }
    bool equals(const char* o) const { return _s == (o ? o : ""); }
    bool equalsIgnoreCase(const String& o) const {
        if (_s.size() != o._s.size()) return false;
        for (size_t i = 0; i < _s.size(); ++i)
            if (lc(_s[i]) != lc(o._s[i])) return false;
        return true;
    }
    int compareTo(const String& o) const { return _s.compare(o._s); }

    // --- search ------------------------------------------------------------
    // Arduino's indexOf returns -1 when not found.
    int indexOf(char c) const { size_t p = _s.find(c); return p == std::string::npos ? -1 : (int)p; }
    int indexOf(char c, unsigned int from) const {
        if (from > _s.size()) return -1;
        size_t p = _s.find(c, from); return p == std::string::npos ? -1 : (int)p;
    }
    int indexOf(const String& n) const { size_t p = _s.find(n._s); return p == std::string::npos ? -1 : (int)p; }
    int indexOf(const String& n, unsigned int from) const {
        if (from > _s.size()) return -1;
        size_t p = _s.find(n._s, from); return p == std::string::npos ? -1 : (int)p;
    }
    int lastIndexOf(char c) const { size_t p = _s.rfind(c); return p == std::string::npos ? -1 : (int)p; }
    int lastIndexOf(const String& n) const { size_t p = _s.rfind(n._s); return p == std::string::npos ? -1 : (int)p; }
    bool startsWith(const String& p) const { return _s.size() >= p._s.size() && _s.compare(0, p._s.size(), p._s) == 0; }
    bool endsWith(const String& p) const { return _s.size() >= p._s.size() && _s.compare(_s.size() - p._s.size(), p._s.size(), p._s) == 0; }

    // --- extraction --------------------------------------------------------
    // Arduino clamps out-of-range indices and returns "" for an invalid span.
    String substring(unsigned int from) const {
        if (from >= _s.size()) return String();
        return String(_s.substr(from));
    }
    String substring(unsigned int from, unsigned int to) const {
        if (from > to) std::swap(from, to);
        if (from >= _s.size()) return String();
        if (to > _s.size()) to = (unsigned int)_s.size();
        return String(_s.substr(from, to - from));
    }

    // --- mutation ----------------------------------------------------------
    void trim() {
        size_t b = 0, e = _s.size();
        while (b < e && isWs(_s[b])) ++b;
        while (e > b && isWs(_s[e - 1])) --e;
        _s = _s.substr(b, e - b);
    }
    void toUpperCase() { for (char& c : _s) c = uc(c); }
    void toLowerCase() { for (char& c : _s) c = lc(c); }
    void remove(unsigned int idx) { if (idx < _s.size()) _s.erase(idx); }
    void remove(unsigned int idx, unsigned int cnt) { if (idx < _s.size()) _s.erase(idx, cnt); }
    void replace(const String& f, const String& r) {
        if (f._s.empty()) return;
        size_t p = 0;
        while ((p = _s.find(f._s, p)) != std::string::npos) { _s.replace(p, f._s.size(), r._s); p += r._s.size(); }
    }
    bool concat(const String& o) { _s += o._s; return true; }

    // --- conversion --------------------------------------------------------
    // Arduino's toInt() is strtol-based and yields 0 for unparseable text.
    long toInt() const { return strtol(_s.c_str(), nullptr, 10); }
    float toFloat() const { return strtof(_s.c_str(), nullptr); }
    double toDouble() const { return strtod(_s.c_str(), nullptr); }

    // --- operators ---------------------------------------------------------
    String& operator+=(const String& o) { _s += o._s; return *this; }
    String& operator+=(const char* o) { if (o) _s += o; return *this; }
    String& operator+=(char c) { _s += c; return *this; }

    const std::string& std_str() const { return _s; }

    friend bool operator==(const String& a, const String& b) { return a._s == b._s; }
    friend bool operator!=(const String& a, const String& b) { return a._s != b._s; }
    friend bool operator<(const String& a, const String& b) { return a._s < b._s; }
    friend bool operator>(const String& a, const String& b) { return a._s > b._s; }
    friend bool operator<=(const String& a, const String& b) { return a._s <= b._s; }
    friend bool operator>=(const String& a, const String& b) { return a._s >= b._s; }
    friend String operator+(const String& a, const String& b) { String r(a); r += b; return r; }

private:
    static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
    static char uc(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c; }
    static bool isWs(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
    std::string _s;
};

// ---------------------------------------------------------------------------
// Misc Arduino core bits the renderer core touches.
// ---------------------------------------------------------------------------
typedef bool boolean;
typedef uint8_t byte;

#ifndef PI
#define PI 3.1415926535897932384626433832795
#endif

// The core calls yield() to feed the FreeRTOS scheduler. On the host it must be
// a genuine no-op so it costs nothing in benchmarks.
static inline void yield() {}

#endif // HOST_SHIM_ARDUINO_H
