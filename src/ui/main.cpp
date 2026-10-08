#include <omni/audio.hpp>
#include <omni/ipc.hpp>
#include <commctrl.h>
#include <uxtheme.h>
#include <shellapi.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <map>
#include <cmath>
#include "snapshot.hpp"
using namespace omni;
namespace
{
constexpr UINT completed = WM_APP + 1;
enum Id
{
    groups_id = 100,
    add_id,
    remove_id,
    name_id,
    save_id,
    refresh_id,
    devices_id,
    gain_id,
    delay_id,
    apply_id,
    test_id,
    mic_id,
    calibrate_id,
    cancel_id,
    settings_id,
    status_id,
    detail_id
};
struct Result
{
    std::string kind;
    Json data;
    std::string error;
};
struct App
{
    HWND window{}, groups{}, name{}, devices{}, gain{}, delay{}, microphone{}, status{}, detail{}, save{},
        calibrate{}, test{};
    HFONT font{};
    UINT dpi{96};
    Config config;
    int selected{-1};
    bool filling{}, dirty{}, refresh_pending{}, smoke{}, save_pending{}, snapshot_mode{};
    int smoke_result{};
    std::vector<Endpoint> outputs, mics;
    Json latest;
    std::mutex tasks_mutex;
    std::condition_variable tasks_cv;
    std::deque<std::function<void()>> tasks;
    bool quit{};
    std::thread worker;
    std::unique_ptr<DeviceWatcher> watcher;
    uint64_t device_revision{};
    ~App()
    {
        {
            std::lock_guard lock(tasks_mutex);
            quit = true;
            tasks.clear();
        }
        tasks_cv.notify_one();
        if (worker.joinable())
            worker.join();
        if (font)
            DeleteObject(font);
    }
    int px(int v) const
    {
        return MulDiv(v, static_cast<int>(dpi), 96);
    }
    HWND control(const wchar_t *cls, const wchar_t *title, DWORD style, int id)
    {
        auto h = CreateWindowExW(0, cls, title, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, window,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr),
                                 nullptr);
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return h;
    }
    void resize_font()
    {
        HFONT old = font;
        font =
            CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        EnumChildWindows(
            window,
            [](HWND child, LPARAM value) -> BOOL {
                SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(value), TRUE);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(font));
        if (old)
            DeleteObject(old);
    }
    void post(std::string kind, std::function<Json()> fn)
    {
        std::lock_guard lock(tasks_mutex);
        tasks.push_back([this, kind = std::move(kind), fn = std::move(fn)] {
            auto r = std::make_unique<Result>();
            r->kind = kind;
            try
            {
                r->data = fn();
            }
            catch (const std::exception &e)
            {
                r->error = e.what();
            }
            if (PostMessageW(window, completed, 0, reinterpret_cast<LPARAM>(r.get())))
                r.release();
        });
        tasks_cv.notify_one();
    }
    void text(HWND h, const std::wstring &s)
    {
        SetWindowTextW(h, s.c_str());
    }
    std::wstring text(HWND h)
    {
        int n = GetWindowTextLengthW(h);
        std::wstring s(n + 1, L'\0');
        GetWindowTextW(h, s.data(), n + 1);
        s.resize(n);
        return s;
    }
    Group *current()
    {
        return selected >= 0 && selected < static_cast<int>(config.groups.size()) ? &config.groups[selected]
                                                                                  : nullptr;
    }
    void edit_enabled(bool enabled)
    {
        save_pending = !enabled;
        for (int id : {groups_id, add_id, remove_id, name_id, save_id, refresh_id, devices_id, gain_id,
                       delay_id, apply_id, calibrate_id})
            EnableWindow(GetDlgItem(window, id), enabled);
    }
    void mark()
    {
        if (filling)
            return;
        dirty = true;
        EnableWindow(save, TRUE);
    }
    void create()
    {
        dpi = GetDpiForWindow(window);
        font =
            CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        groups =
            control(L"LISTBOX", L"출력 그룹", LBS_NOTIFY | WS_VSCROLL | WS_TABSTOP | WS_BORDER, groups_id);
        control(L"BUTTON", L"+ 그룹", WS_TABSTOP, add_id);
        control(L"BUTTON", L"삭제", WS_TABSTOP, remove_id);
        control(L"STATIC", L"그룹 이름", 0, 201);
        name = control(L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, name_id);
        SendMessageW(name, EM_SETLIMITTEXT, 64, 0);
        save = control(L"BUTTON", L"저장", WS_TABSTOP | BS_DEFPUSHBUTTON, save_id);
        control(L"BUTTON", L"새로 고침", WS_TABSTOP, refresh_id);
        devices =
            control(WC_LISTVIEWW, L"그룹 출력 기기",
                    WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, devices_id);
        ListView_SetExtendedListViewStyle(devices,
                                          LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        const wchar_t *headings[] = {L"출력 기기", L"연결", L"볼륨", L"추가 지연", L"보정"};
        for (int i = 0; i < 5; ++i)
        {
            LVCOLUMNW c{};
            c.mask = LVCF_TEXT | LVCF_WIDTH;
            c.pszText = const_cast<LPWSTR>(headings[i]);
            c.cx = px(i == 0 ? 280 : 90);
            ListView_InsertColumn(devices, i, &c);
        }
        control(L"STATIC", L"볼륨 (%)", 0, 202);
        gain = control(L"EDIT", L"100", WS_BORDER | WS_TABSTOP | ES_NUMBER, gain_id);
        control(L"STATIC", L"추가 지연 (ms)", 0, 203);
        delay = control(L"EDIT", L"0.0", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, delay_id);
        control(L"BUTTON", L"적용", WS_TABSTOP, apply_id);
        test = control(L"BUTTON", L"기기 확인음", WS_TABSTOP, test_id);
        control(L"STATIC", L"측정 마이크", 0, 204);
        microphone =
            control(WC_COMBOBOXW, L"측정 마이크", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, mic_id);
        calibrate = control(L"BUTTON", L"자동 보정", WS_TABSTOP, calibrate_id);
        control(L"BUTTON", L"취소", WS_TABSTOP, cancel_id);
        detail = control(L"STATIC", L"", SS_LEFT, detail_id);
        status = control(L"STATIC", L"기기를 불러오는 중…", SS_LEFT, status_id);
        control(L"BUTTON", L"Windows 소리 설정", WS_TABSTOP, settings_id);
        SendMessageW(gain, EM_SETLIMITTEXT, 3, 0);
        SendMessageW(delay, EM_SETLIMITTEXT, 8, 0);
        EnableWindow(save, FALSE);
        worker = std::thread([this] {
            ComScope com;
            for (;;)
            {
                std::function<void()> fn;
                {
                    std::unique_lock lock(tasks_mutex);
                    tasks_cv.wait(lock, [&] { return quit || !tasks.empty(); });
                    if (quit)
                        break;
                    fn = std::move(tasks.front());
                    tasks.pop_front();
                }
                fn();
            }
        });
        if (!smoke)
        {
            watcher = std::make_unique<DeviceWatcher>();
            device_revision = watcher->revision();
            reload();
            SetTimer(window, 1, 1000, nullptr);
        }
        else
        {
            config.groups.push_back(Group{new_id(), "책상", {{"demo-monitor", 0.7, 0, {}}}, 200});
            outputs.push_back({"demo-monitor", "DELL S2725DS · 1", "", "", "", false, false, true});
            outputs.push_back({"demo-speaker", "스피커 (Realtek Audio)", "", "", "", false, false, true});
            selected = 0;
            fill_groups();
            fill();
            text(status, L"UI smoke test — no driver installed, no audio played");
            SetTimer(window, 2, 300, nullptr);
        }
    }
    void layout()
    {
        RECT r;
        GetClientRect(window, &r);
        int w = MulDiv(r.right, 96, dpi), h = MulDiv(r.bottom, 96, dpi);
        auto place = [&](int id, int x, int y, int width, int height) {
            MoveWindow(GetDlgItem(window, id), px(x), px(y), px(std::max(width, 1)), px(std::max(height, 1)),
                       TRUE);
        };
        place(groups_id, 16, 16, 190, h - 122);
        place(add_id, 16, h - 96, 90, 30);
        place(remove_id, 116, h - 96, 90, 30);
        int x = 226, right = w - 242;
        place(201, x, 18, 76, 26);
        place(name_id, x + 82, 16, right - 268, 30);
        place(save_id, w - 188, 16, 72, 30);
        place(refresh_id, w - 106, 16, 90, 30);
        place(devices_id, x, 60, right, h - 310);
        place(202, x, h - 236, 76, 26);
        place(gain_id, x + 78, h - 240, 54, 30);
        place(203, x + 148, h - 236, 104, 26);
        place(delay_id, x + 256, h - 240, 72, 30);
        place(apply_id, x + 340, h - 240, 62, 30);
        place(test_id, x + 416, h - 240, 110, 30);
        place(204, x, h - 188, 94, 26);
        place(mic_id, x + 102, h - 192, right - 288, 260);
        place(calibrate_id, w - 192, h - 192, 96, 30);
        place(cancel_id, w - 86, h - 192, 70, 30);
        place(detail_id, x, h - 144, right, 66);
        place(status_id, 16, h - 52, w - 220, 40);
        place(settings_id, w - 188, h - 54, 172, 32);
        ListView_SetColumnWidth(devices, 0, px(std::max(160, right - 375)));
        for (int i = 1; i < 5; ++i)
            ListView_SetColumnWidth(devices, i, px(i == 4 ? 112 : 83));
    }
    void fill_groups()
    {
        filling = true;
        SendMessageW(groups, LB_RESETCONTENT, 0, 0);
        for (const auto &g : config.groups)
            SendMessageW(groups, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(g.name).c_str()));
        SendMessageW(groups, LB_SETCURSEL, selected, 0);
        filling = false;
    }
    void rowtext(int i, int col, const std::wstring &s)
    {
        ListView_SetItemText(devices, i, col, const_cast<LPWSTR>(s.c_str()));
    }
    void fill()
    {
        filling = true;
        auto *g = current();
        text(name, g ? wide(g->name) : L"");
        ListView_DeleteAllItems(devices);
        std::map<std::string, int> duplicate, total;
        for (auto &e : outputs)
            ++total[e.name];
        for (size_t i = 0; i < outputs.size(); ++i)
        {
            auto &e = outputs[i];
            std::wstring label = wide(e.name);
            if (total[e.name] > 1)
                label += L" · " + std::to_wstring(++duplicate[e.name]);
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = static_cast<int>(i);
            item.pszText = label.data();
            ListView_InsertItem(devices, &item);
            const Member *m = nullptr;
            if (g)
                for (const auto &v : g->members)
                    if (v.endpoint_id == e.id)
                        m = &v;
            ListView_SetCheckState(devices, static_cast<int>(i), m != nullptr);
            rowtext(static_cast<int>(i), 1, !e.active ? L"연결 끊김" : e.bluetooth ? L"Bluetooth" : L"유선");
            rowtext(static_cast<int>(i), 2,
                    std::to_wstring(static_cast<int>(std::lround((m ? m->gain : 1) * 100))) + L"%");
            wchar_t value[40];
            swprintf_s(value, L"%.2f ms", m ? m->delay_ms : 0);
            rowtext(static_cast<int>(i), 3, value);
            rowtext(static_cast<int>(i), 4,
                    m && m->calibration ? (m->calibration->verified ? L"실측 기록 있음" : L"목표 미달")
                                        : L"미측정");
        }
        if (!outputs.empty())
            ListView_SetItemState(devices, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        EnableWindow(name, g != nullptr);
        EnableWindow(devices, g != nullptr);
        EnableWindow(calibrate, g != nullptr);
        filling = false;
        select_row();
        show_detail();
    }
    void select_row()
    {
        int row = ListView_GetNextItem(devices, -1, LVNI_SELECTED);
        if (row < 0 || row >= static_cast<int>(outputs.size()))
            return;
        Member m{outputs[row].id};
        if (auto *g = current())
            for (auto &v : g->members)
                if (v.endpoint_id == m.endpoint_id)
                    m = v;
        text(gain, std::to_wstring(static_cast<int>(std::lround(m.gain * 100))));
        wchar_t b[40];
        swprintf_s(b, L"%.3f", m.delay_ms);
        text(delay, b);
        show_detail();
    }
    void show_detail()
    {
        auto *g = current();
        if (!g)
        {
            text(detail, L"그룹을 추가하고 출력 기기를 선택하세요.");
            return;
        }
        int row = ListView_GetNextItem(devices, -1, LVNI_SELECTED);
        std::wstring estimate;
        if (row >= 0 && row < static_cast<int>(outputs.size()) && latest.contains("engine"))
            for (const auto &group : latest["engine"].value("groups", Json::array()))
                if (group.value("id", std::string{}) == g->id)
                    for (const auto &member : group.value("members", Json::array()))
                        if (member.value("endpointId", std::string{}) == outputs[row].id)
                        {
                            wchar_t value[160];
                            swprintf_s(value, L"\n현재 클럭 위치 보정 추정 %.3f ms · 음향 실측 아님",
                                       member.value("estimatedCorrectionMs", 0.0));
                            estimate = value;
                        }
        if (row >= 0 && row < static_cast<int>(outputs.size()))
            for (auto &m : g->members)
                if (m.endpoint_id == outputs[row].id && m.calibration)
                {
                    auto &a = *m.calibration;
                    wchar_t b[180];
                    swprintf_s(b, L"마지막 실측 최대 편차 %.3f ms · 불확실성 ±%.3f ms\n",
                               a.verified_spread_ms, a.uncertainty_ms);
                    text(detail, std::wstring(b) + wide(a.measured_at) +
                                     (a.verified ? L" · 실측 목표 통과" : L" · 실측 목표 미달") + estimate);
                    return;
                }
        text(detail,
             std::wstring(L"마이크를 청취 위치에 고정하세요. 자동 보정은 테스트음을 재생하며 수동 지연을 "
                          L"초기화합니다.\n1 "
                          L"ms는 실측 목표이며, 현재 실제 소리의 오차는 재측정 전까지 확인되지 않습니다.") +
                 estimate);
    }
    void reload()
    {
        if (refresh_pending)
            return;
        refresh_pending = true;
        text(status, L"기기를 불러오는 중…");
        post("load", [] {
            Json result;
            result["outputs"] = endpoints(false, true);
            result["microphones"] = endpoints(true);
            try
            {
                result["service"] = request(service_pipe, {{"op", "load"}}, 1500);
            }
            catch (const std::exception &e)
            {
                result["serviceError"] = e.what();
            }
            return result;
        });
    }
    void save_config(bool calibrate_after = false)
    {
        if (!current())
            return;
        current()->name = utf8(text(name));
        validate(config);
        edit_enabled(false);
        text(status, L"저장 중…");
        auto copy = config;
        post(calibrate_after ? "save-calibrate" : "save",
             [copy] { return request(service_pipe, {{"op", "save"}, {"config", copy}}); });
    }
    void begin_calibration()
    {
        auto *g = current();
        if (!g)
            return;
        int mic = static_cast<int>(SendMessageW(microphone, CB_GETCURSEL, 0, 0));
        if (mic < 0 || mic >= static_cast<int>(mics.size()))
            throw std::runtime_error("측정할 PC 또는 USB 마이크를 선택하세요.");
        auto group = g->id;
        auto id = mics[mic].id;
        text(status, L"보정 준비 중…");
        post("command", [group, id] {
            return request(engine_pipe(), {{"op", "calibrate"}, {"groupId", group}, {"microphoneId", id}});
        });
    }
    void tick()
    {
        if (refresh_pending)
            return;
        if (watcher && watcher->revision() != device_revision)
        {
            device_revision = watcher->revision();
            reload();
            return;
        }
        refresh_pending = true;
        post("status", [] { return request(engine_pipe(), {{"op", "status"}}, 1200); });
    }
    static Endpoint endpoint_from(const Json &j)
    {
        return {j.at("id"),          j.at("name"),      j.at("instance"), j.at("groupId"),
                j.at("fingerprint"), j.at("bluetooth"), j.at("capture"),  j.at("active")};
    }
    void result(Result &r)
    {
        if (r.kind == "load" || r.kind == "status")
            refresh_pending = false;
        if (!r.error.empty())
        {
            text(status, wide(r.error));
            if (r.kind == "save" || r.kind == "save-calibrate")
                edit_enabled(true);
            return;
        }
        if (r.kind == "load")
        {
            outputs.clear();
            mics.clear();
            for (const auto &j : r.data["outputs"])
            {
                auto e = endpoint_from(j);
                if (e.group_id.empty())
                    outputs.push_back(e);
            }
            for (const auto &j : r.data["microphones"])
            {
                auto e = endpoint_from(j);
                if (!e.bluetooth && e.active)
                    mics.push_back(e);
            }
            if (r.data.contains("service") && !dirty)
            {
                std::string old = current() ? current()->id : "";
                config = r.data["service"]["config"].get<Config>();
                selected = config.groups.empty() ? -1 : 0;
                for (size_t i = 0; i < config.groups.size(); ++i)
                    if (config.groups[i].id == old)
                        selected = static_cast<int>(i);
                fill_groups();
            }
            for (const auto &g : config.groups)
                for (const auto &m : g.members)
                    if (std::none_of(outputs.begin(), outputs.end(),
                                     [&](const Endpoint &e) { return e.id == m.endpoint_id; }))
                        outputs.push_back(
                            {m.endpoint_id, "연결되지 않은 저장 기기", "", "", "", false, false, false});
            std::erase_if(outputs, [&](const Endpoint &e) {
                if (e.active)
                    return false;
                for (const auto &g : config.groups)
                    for (const auto &m : g.members)
                        if (m.endpoint_id == e.id)
                            return false;
                return true;
            });
            std::sort(outputs.begin(), outputs.end(), [](const Endpoint &a, const Endpoint &b) {
                return a.name == b.name ? a.id < b.id : a.name < b.name;
            });
            SendMessageW(microphone, CB_RESETCONTENT, 0, 0);
            for (auto &e : mics)
                SendMessageW(microphone, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(e.name).c_str()));
            if (!mics.empty())
                SendMessageW(microphone, CB_SETCURSEL, 0, 0);
            fill();
            text(status, r.data.contains("serviceError")
                             ? L"장치 관리 서비스가 없습니다. 설치 프로그램을 먼저 실행하세요."
                             : L"그룹을 저장한 뒤 Windows 소리 출력 목록에서 선택하세요.");
        }
        else if (r.kind == "save" || r.kind == "save-calibrate")
        {
            edit_enabled(true);
            config = r.data["config"].get<Config>();
            dirty = false;
            EnableWindow(save, FALSE);
            fill_groups();
            fill();
            text(status, L"저장됨 · Windows 출력 장치 준비 중");
            if (r.kind == "save-calibrate")
                begin_calibration();
        }
        else if (r.kind == "status")
        {
            auto &job = r.data["job"];
            auto state = job.value("state", std::string{});
            bool running = state == "running";
            EnableWindow(calibrate, !running && !save_pending && current());
            EnableWindow(test, !running);
            EnableWindow(GetDlgItem(window, cancel_id), running);
            if (running)
                text(status, wide(job.value("progress", std::string("측정 중"))));
            else if (state == "error")
                text(status, wide(job.value("message", std::string{})));
            else if (state == "complete" && job.contains("result") &&
                     latest.value("job", Json::object()) != job)
            {
                text(status, job["result"].value("passed", false) ? L"보정 완료 · 실측 목표 통과"
                                                                  : L"보정 완료 · 1 ms 목표 미달");
                if (!dirty)
                    reload();
            }
            else if (!dirty)
            {
                auto e = r.data["engine"];
                if (e.value("state", std::string{}) == "service-unavailable")
                    text(status, L"관리 서비스에 연결할 수 없습니다. 설치 상태를 확인하세요.");
                else if (current())
                {
                    for (const auto &g : e.value("groups", Json::array()))
                        if (g.value("id", std::string{}) == current()->id)
                        {
                            auto s = g.value("state", std::string{});
                            if (s == "driver-unavailable")
                                text(status, L"가상 출력 드라이버가 준비되지 않았습니다. 서명된 드라이버 "
                                             L"설치가 필요합니다.");
                            else
                            {
                                wchar_t b[150];
                                swprintf_s(b, L"%s · 그룹 버퍼 %.0f ms",
                                           s == "playing"     ? L"재생 중"
                                           : s == "buffering" ? L"소리 대기 / 버퍼 준비"
                                                              : L"기기 확인 필요",
                                           g.value("latencyMs", 0.0));
                                text(status, b);
                            }
                            for (const auto &m : g.value("members", Json::array()))
                                for (size_t i = 0; i < outputs.size(); ++i)
                                    if (m.value("endpointId", std::string{}) == outputs[i].id)
                                    {
                                        rowtext(static_cast<int>(i), 4,
                                                m.value("calibrationValid", false) ? L"보정 적용"
                                                                                   : L"재보정 필요");
                                    }
                        }
                }
            }
            latest = r.data;
            show_detail();
        }
    }
    void command(int id, int notification)
    {
        try
        {
            if (id == groups_id && notification == LBN_SELCHANGE)
            {
                if (auto *g = current())
                    g->name = utf8(text(name));
                selected = static_cast<int>(SendMessageW(groups, LB_GETCURSEL, 0, 0));
                fill();
            }
            else if (id == name_id && notification == EN_CHANGE)
            {
                if (!filling && current())
                    current()->name = utf8(text(name));
                mark();
            }
            else if (id == add_id)
            {
                if (config.groups.size() >= max_groups)
                    throw std::runtime_error("그룹은 최대 16개입니다.");
                std::string n = "새 그룹";
                unsigned suffix = 1;
                while (std::any_of(config.groups.begin(), config.groups.end(),
                                   [&](const Group &g) { return g.name == n; }))
                    n = "새 그룹 " + std::to_string(++suffix);
                config.groups.push_back({new_id(), n, {}, 200});
                selected = static_cast<int>(config.groups.size() - 1);
                mark();
                fill_groups();
                fill();
            }
            else if (id == remove_id && current())
            {
                config.groups.erase(config.groups.begin() + selected);
                selected = config.groups.empty() ? -1 : 0;
                mark();
                fill_groups();
                fill();
                auto copy = config;
                edit_enabled(false);
                post("save", [copy] { return request(service_pipe, {{"op", "save"}, {"config", copy}}); });
            }
            else if (id == save_id)
                save_config();
            else if (id == refresh_id)
                reload();
            else if (id == apply_id)
            {
                int row = ListView_GetNextItem(devices, -1, LVNI_SELECTED);
                auto *g = current();
                if (!g || row < 0)
                    return;
                size_t used1{}, used2{};
                auto gs = text(gain), ds = text(delay);
                double value = std::stod(gs, &used1) / 100, ms = std::stod(ds, &used2);
                if (used1 != gs.size() || used2 != ds.size() || !std::isfinite(value) || !std::isfinite(ms) ||
                    value < 0 || value > 1 || ms < 0 || ms > 1000)
                    throw std::runtime_error("볼륨 0–100%, 추가 지연 0–1000 ms 범위로 입력하세요.");
                for (auto &m : g->members)
                    if (m.endpoint_id == outputs[row].id)
                    {
                        m.gain = value;
                        m.delay_ms = ms;
                        mark();
                        fill();
                        return;
                    }
                throw std::runtime_error("먼저 해당 출력 기기를 체크하세요.");
            }
            else if (id == test_id)
            {
                int row = ListView_GetNextItem(devices, -1, LVNI_SELECTED);
                if (row < 0)
                    return;
                auto ep = outputs[row].id;
                post("command",
                     [ep] { return request(engine_pipe(), {{"op", "test"}, {"endpointId", ep}}); });
            }
            else if (id == calibrate_id)
            {
                if (dirty)
                    save_config(true);
                else
                    begin_calibration();
            }
            else if (id == cancel_id)
                post("command", [] { return request(engine_pipe(), {{"op", "cancel"}}); });
            else if (id == settings_id)
                ShellExecuteW(window, L"open", L"ms-settings:sound", nullptr, nullptr, SW_SHOWNORMAL);
        }
        catch (const std::exception &e)
        {
            text(status, wide(e.what()));
        }
    }
};
LRESULT CALLBACK procedure(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    auto *app = reinterpret_cast<App *>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (msg == WM_NCCREATE)
    {
        app = static_cast<App *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        app->window = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app)
        return DefWindowProcW(h, msg, w, l);
    try
    {
        switch (msg)
        {
        case WM_CREATE:
            app->create();
            return 0;
        case WM_SIZE:
            app->layout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto *p = reinterpret_cast<MINMAXINFO *>(l);
            p->ptMinTrackSize = {app->px(980), app->px(620)};
            return 0;
        }
        case WM_COMMAND:
            app->command(LOWORD(w), HIWORD(w));
            return 0;
        case WM_NOTIFY: {
            auto *p = reinterpret_cast<NMHDR *>(l);
            if (p->idFrom == devices_id && p->code == LVN_ITEMCHANGED && !app->filling)
            {
                auto *n = reinterpret_cast<NMLISTVIEW *>(l);
                auto *g = app->current();
                if (g && n->iItem >= 0 && n->iItem < static_cast<int>(app->outputs.size()) &&
                    ((n->uOldState ^ n->uNewState) & LVIS_STATEIMAGEMASK))
                {
                    const auto &id = app->outputs[n->iItem].id;
                    auto it = std::find_if(g->members.begin(), g->members.end(),
                                           [&](const Member &m) { return m.endpoint_id == id; });
                    if (ListView_GetCheckState(app->devices, n->iItem))
                    {
                        if (it == g->members.end())
                            g->members.push_back({id});
                    }
                    else if (it != g->members.end())
                        g->members.erase(it);
                    app->mark();
                }
                if ((n->uOldState ^ n->uNewState) & LVIS_SELECTED)
                    app->select_row();
            }
            return 0;
        }
        case completed: {
            std::unique_ptr<Result> r(reinterpret_cast<Result *>(l));
            app->result(*r);
            return 0;
        }
        case WM_TIMER:
            if (w == 2)
            {
                if (ListView_GetItemCount(app->devices) != 2 || !ListView_GetCheckState(app->devices, 0) ||
                    SendMessageW(app->groups, LB_GETCOUNT, 0, 0) != 1)
                    app->smoke_result = 1;
                if (app->snapshot_mode)
                    save_ui_snapshot(h, executable_dir().parent_path() / L"ui-smoke.png");
                DestroyWindow(h);
            }
            else
                app->tick();
            return 0;
        case WM_DPICHANGED: {
            app->dpi = HIWORD(w);
            app->resize_font();
            auto *r = reinterpret_cast<RECT *>(l);
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_CTLCOLORSTATIC:
            SetBkMode(reinterpret_cast<HDC>(w), TRANSPARENT);
            SetTextColor(reinterpret_cast<HDC>(w), GetSysColor(COLOR_WINDOWTEXT));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        case WM_CLOSE:
            if (app->dirty && MessageBoxW(h, L"저장하지 않은 변경을 버리고 닫을까요?", L"OmniSound",
                                          MB_YESNO | MB_ICONQUESTION) != IDYES)
                return 0;
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            KillTimer(h, 1);
            KillTimer(h, 2);
            PostQuitMessage(0);
            return 0;
        }
    }
    catch (const std::exception &e)
    {
        if (app->status)
            SetWindowTextW(app->status, wide(e.what()).c_str());
        else
            return -1;
    }
    return DefWindowProcW(h, msg, w, l);
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR args, int show)
{
    try
    {
        ComScope com;
        INITCOMMONCONTROLSEX cc{sizeof(cc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&cc);
        App app;
        app.smoke = std::wstring(args).find(L"--smoke-test") != std::wstring::npos;
        app.snapshot_mode = app.smoke && std::wstring(args).find(L"--snapshot") != std::wstring::npos;
        WNDCLASSEXW c{sizeof(c)};
        c.hInstance = instance;
        c.lpfnWndProc = procedure;
        c.lpszClassName = L"OmniSound.Window";
        c.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        c.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        c.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassExW(&c);
        HWND h = CreateWindowExW(WS_EX_CONTROLPARENT, c.lpszClassName, L"OmniSound", WS_OVERLAPPEDWINDOW,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 1080, 700, nullptr, nullptr, instance, &app);
        if (!h)
            return 1;
        SetWindowPos(h, nullptr, 0, 0, app.px(1080), app.px(700), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        if (app.snapshot_mode)
            ShowWindow(h, SW_SHOWNOACTIVATE);
        if (!app.smoke)
        {
            try
            {
                SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
                SC_HANDLE service =
                    manager ? OpenServiceW(manager, L"OmniSound", SERVICE_QUERY_STATUS) : nullptr;
                SERVICE_STATUS state{};
                bool running =
                    service && QueryServiceStatus(service, &state) && state.dwCurrentState == SERVICE_RUNNING;
                if (service)
                    CloseServiceHandle(service);
                if (manager)
                    CloseServiceHandle(manager);
                if (running)
                    start_sibling(L"OmniSoundEngine.exe");
            }
            catch (const std::exception &e)
            {
                log_event("start-engine", e.what());
            }
            ShowWindow(h, show);
        }
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            if (!IsDialogMessageW(h, &msg))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        return app.smoke_result;
    }
    catch (const std::exception &e)
    {
        MessageBoxW(nullptr, wide(e.what()).c_str(), L"OmniSound", MB_ICONERROR);
        return 1;
    }
}
