// procwatch.cpp — Event-driven executable detection.
//
// Subscribes to WMI's Win32_ProcessStartTrace (backed by the kernel's process
// ETW events; requires admin, which the app already has) on a dedicated
// thread. When a process starts whose name matches a monitored executable or a
// profile trigger, the monitor thread is woken so auto-stop / profile switching
// happen immediately instead of on the next poll. Polling stays as the fallback:
// if the subscription can't be created (WMI disabled, policy, Wine, ...), we log
// once and exit, and nothing else changes.
#include "app.h"

#include <objbase.h>
#include <wbemidl.h>

namespace {

template <typename T>
struct com_ptr {
    T* p = nullptr;
    com_ptr() = default;
    com_ptr(const com_ptr&) = delete;
    com_ptr& operator=(const com_ptr&) = delete;
    ~com_ptr() { reset(); }
    void reset() { if (p) { p->Release(); p = nullptr; } }
    T** put() { reset(); return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// Win32_ProcessStartTrace.ProcessName comes from the kernel's ImageFileName,
// which is truncated to 14 characters + NUL. Accept an exact match, or a
// truncated event name that is a prefix of the monitored name.
bool name_matches(const wchar_t* event_name, size_t event_len, const std::wstring& wanted) {
    if (wanted.empty() || event_len == 0) return false;
    if (event_len == wanted.size()) return _wcsicmp(event_name, wanted.c_str()) == 0;
    if (event_len >= 14 && event_len < wanted.size())
        return _wcsnicmp(event_name, wanted.c_str(), event_len) == 0;
    return false;
}

bool is_interesting_locked(App& self, const wchar_t* name, size_t len) {
    for (const auto& up : self.items[KIND_EXE].v) {
        if (up && name_matches(name, len, up->name)) return true;
    }
    if (self.prof.watch_keys_sp) {
        for (const auto& keys : *self.prof.watch_keys_sp)
            for (const auto& k : keys)
                if (name_matches(name, len, k)) return true;
    }
    return false;
}

void log_unavailable(App& self, const wchar_t* step, HRESULT hr) {
    wchar_t msg[200];
    StringCchPrintfW(msg, 200,
        L"Program-start events unavailable (%s, 0x%08lX); executables are checked by polling only.",
        step, (unsigned long)hr);
    post_log(self, msg);
}

HRESULT set_blanket(IUnknown* p) {
    return CoSetProxyBlanket(p, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
                             RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                             NULL, EOAC_NONE);
}

}  // namespace

void proc_watch_thread_main(std::stop_token st, App* self_ptr) {
    App& self = *self_ptr;

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) { log_unavailable(self, L"COM init", hr); return; }
    struct ComUninit { ~ComUninit() { CoUninitialize(); } } com_uninit;

    {
        com_ptr<IWbemLocator> locator;
        hr = CoCreateInstance(CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER, IID_IWbemLocator,
                              (void**)locator.put());
        if (FAILED(hr)) { log_unavailable(self, L"WMI locator", hr); return; }

        com_ptr<IWbemServices> svc;
        BSTR ns = SysAllocString(L"ROOT\\CIMV2");
        hr = locator->ConnectServer(ns, NULL, NULL, NULL, 0, NULL, NULL, svc.put());
        SysFreeString(ns);
        if (FAILED(hr)) { log_unavailable(self, L"WMI connect", hr); return; }
        // Event queries need impersonation; set it on the proxy rather than
        // calling the process-wide CoInitializeSecurity.
        set_blanket((IUnknown*)svc.p);

        com_ptr<IEnumWbemClassObject> events;
        BSTR lang = SysAllocString(L"WQL");
        BSTR query = SysAllocString(L"SELECT ProcessName FROM Win32_ProcessStartTrace");
        hr = svc->ExecNotificationQuery(lang, query,
                                        WBEM_FLAG_RETURN_IMMEDIATELY | WBEM_FLAG_FORWARD_ONLY,
                                        NULL, events.put());
        SysFreeString(lang);
        SysFreeString(query);
        if (FAILED(hr)) { log_unavailable(self, L"subscribe", hr); return; }
        set_blanket((IUnknown*)events.p);

        // Poll the semi-synchronous enumerator with a short timeout so stop
        // requests are noticed promptly.
        constexpr long kNextTimeoutMs = 500;
        while (!st.stop_requested()) {
            com_ptr<IWbemClassObject> obj;
            ULONG got = 0;
            hr = events->Next(kNextTimeoutMs, 1, obj.put(), &got);
            if (hr == WBEM_S_TIMEDOUT) continue;
            if (FAILED(hr)) { log_unavailable(self, L"event stream", hr); return; }
            if (got == 0 || !obj) continue;

            VARIANT v;
            VariantInit(&v);
            if (SUCCEEDED(obj->Get(L"ProcessName", 0, &v, NULL, NULL)) && v.vt == VT_BSTR && v.bstrVal) {
                const wchar_t* name = v.bstrVal;
                const size_t len = SysStringLen(v.bstrVal);
                bool wake = false;
                {
                    ModelReadGuard lk(self);
                    wake = is_interesting_locked(self, name, len);
                }
                // The monitor's own snapshot does the matching/enforcement; we
                // only make it run now. Bursts are coalesced by monitor_wait().
                if (wake) monitor_wake(self);
            }
            VariantClear(&v);
        }
    }
}
