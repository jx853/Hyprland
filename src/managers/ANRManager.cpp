#include "ANRManager.hpp"
#include "../helpers/fs/FsUtils.hpp"
#include "../debug/Log.hpp"
#include "../macros.hpp"
#include "HookSystemManager.hpp"
#include "../Compositor.hpp"
#include "../protocols/XDGShell.hpp"
#include "./eventLoop/EventLoopManager.hpp"
#include "../config/ConfigValue.hpp"
#include "../xwayland/XSurface.hpp"
#include "./EventManager.hpp"
#include <string>

using namespace Hyprutils::OS;

static constexpr auto TIMER_TIMEOUT = std::chrono::milliseconds(1500);

CANRManager::CANRManager() {
    if (!NFsUtils::executableExistsInPath("hyprland-dialog")) {
        Debug::log(ERR, "hyprland-dialog missing from PATH, cannot start ANRManager");
        return;
    }

    m_timer = makeShared<CEventLoopTimer>(TIMER_TIMEOUT, [this](SP<CEventLoopTimer> self, void* data) { onTick(); }, this);
    g_pEventLoopManager->addTimer(m_timer);

    m_active = true;

    static auto P = g_pHookSystem->hookDynamic("openWindow", [this](void* self, SCallbackInfo& info, std::any data) {
        auto window = std::any_cast<PHLWINDOW>(data);

        for (const auto& d : m_data) {
            if (d->fitsWindow(window))
                return;
        }

        m_data.emplace_back(makeShared<SANRData>(window));
    });

    static auto P1 = g_pHookSystem->hookDynamic("closeWindow", [this](void* self, SCallbackInfo& info, std::any data) {
        auto window = std::any_cast<PHLWINDOW>(data);

        for (const auto& d : m_data) {
            if (!d->fitsWindow(window))
                continue;

            // kill the dialog, act as if we got a "ping" in case there's more than one
            // window from this client, in which case the dialog will re-appear.
            d->killDialog();
            d->missedResponses = 0;
            d->dialogSaidWait  = false;
            return;
        }
    });

    m_timer->updateTimeout(TIMER_TIMEOUT);
}

void CANRManager::onTick() {
    static auto PENABLEANR    = CConfigValue<Hyprlang::INT>("misc:enable_anr_dialog");
    static auto PANRTHRESHOLD = CConfigValue<Hyprlang::INT>("misc:anr_missed_pings");

    for (auto& data : m_data) {
        PHLWINDOW firstWindow;
        int       count = 0;
        for (const auto& w : g_pCompositor->m_windows) {
            if (!w->m_isMapped)
                continue;

            if (!data->fitsWindow(w))
                continue;

            count++;
            if (!firstWindow)
                firstWindow = w;

            *w->m_notRespondingTint = 0.2F;
        }

        if (count == 0) {
            if (data->wasNotResponding) {
                g_pEventManager->postEvent(SHyprIPCEvent{.event = "anrrecovered", .data = std::to_string(data->getPid())});
                data->wasNotResponding = false;
            }
            continue;
        }

        if (data->missedResponses >= *PANRTHRESHOLD) {
            data->wasNotResponding = true;

            if (!data->isRunning() && !data->dialogSaidWait) {
                if (data->missedResponses == *PANRTHRESHOLD)
                    g_pEventManager->postEvent(SHyprIPCEvent{.event = "anr", .data = std::to_string(data->getPid())});

                if (*PENABLEANR) {
                    data->runDialog("Application Not Responding", firstWindow->m_title, firstWindow->m_class, data->getPid());
                }
            }
        } else if (data->isRunning())
            data->killDialog();

        if (data->missedResponses == 0)
            data->dialogSaidWait = false;

        data->missedResponses++;

        data->ping();
    }

    m_timer->updateTimeout(TIMER_TIMEOUT);
}

void CANRManager::onResponse(SP<CXDGWMBase> wmBase) {
    const auto DATA = dataFor(wmBase);

    if (!DATA)
        return;

    onResponse(DATA);
}

void CANRManager::onResponse(SP<CXWaylandSurface> pXwaylandSurface) {
    const auto DATA = dataFor(pXwaylandSurface);

    if (!DATA)
        return;

    onResponse(DATA);
}

void CANRManager::onResponse(SP<CANRManager::SANRData> data) {
    static auto PANRTHRESHOLD = CConfigValue<Hyprlang::INT>("misc:anr_missed_pings");

    if (data->wasNotResponding && data->missedResponses >= *PANRTHRESHOLD) {
        g_pEventManager->postEvent(SHyprIPCEvent{.event = "anrrecovered", .data = std::to_string(data->getPid())});
        data->wasNotResponding = false;
    }

    data->missedResponses = 0;
    if (data->isRunning())
        data->killDialog();
}

bool CANRManager::isNotResponding(PHLWINDOW pWindow) {
    const auto DATA = dataFor(pWindow);

    if (!DATA)
        return false;

    return isNotResponding(DATA);
}

bool CANRManager::isNotResponding(SP<CANRManager::SANRData> data) {
    static auto PANRTHRESHOLD = CConfigValue<Hyprlang::INT>("misc:anr_missed_pings");
    return data->missedResponses > *PANRTHRESHOLD;
}

SP<CANRManager::SANRData> CANRManager::dataFor(PHLWINDOW pWindow) {
    auto it = m_data.end();
    if (pWindow->m_xwaylandSurface)
        it = std::ranges::find_if(m_data, [&pWindow](const auto& data) { return data->xwaylandSurface && data->xwaylandSurface == pWindow->m_xwaylandSurface; });
    else if (pWindow->m_xdgSurface)
        it = std::ranges::find_if(m_data, [&pWindow](const auto& data) { return data->xdgBase && data->xdgBase == pWindow->m_xdgSurface->m_owner; });
    return it == m_data.end() ? nullptr : *it;
}

SP<CANRManager::SANRData> CANRManager::dataFor(SP<CXDGWMBase> wmBase) {
    auto it = std::ranges::find_if(m_data, [&wmBase](const auto& data) { return data->xdgBase && data->xdgBase == wmBase; });
    return it == m_data.end() ? nullptr : *it;
}

SP<CANRManager::SANRData> CANRManager::dataFor(SP<CXWaylandSurface> pXwaylandSurface) {
    auto it = std::ranges::find_if(m_data, [&pXwaylandSurface](const auto& data) { return data->xwaylandSurface && data->xwaylandSurface == pXwaylandSurface; });
    return it == m_data.end() ? nullptr : *it;
}

CANRManager::SANRData::SANRData(PHLWINDOW pWindow) :
    xwaylandSurface(pWindow->m_xwaylandSurface), xdgBase(pWindow->m_xdgSurface ? pWindow->m_xdgSurface->m_owner : WP<CXDGWMBase>{}) {

    // Cache the PID at creation time because it is unavalable after a window is closed. This is needed to send anrrecovered if a non responding app is killed
    if (xdgBase) {
        wl_client_get_credentials(xdgBase->client(), &cachedPid, nullptr, nullptr);
    } else if (xwaylandSurface) {
        cachedPid = xwaylandSurface->m_pid;
    }
}

CANRManager::SANRData::~SANRData() {
    if (dialogBox && dialogBox->isRunning())
        killDialog();
}

void CANRManager::SANRData::runDialog(const std::string& title, const std::string& appName, const std::string appClass, pid_t dialogWmPID) {
    if (dialogBox && dialogBox->isRunning())
        killDialog();

    dialogBox = CAsyncDialogBox::create(title,
                                        std::format("Application {} with class of {} is not responding.\nWhat do you want to do with it?", appName.empty() ? "unknown" : appName,
                                                    appClass.empty() ? "unknown" : appClass),
                                        std::vector<std::string>{"Terminate", "Wait"});

    dialogBox->open()->then([dialogWmPID, this](SP<CPromiseResult<std::string>> r) {
        if (r->hasError()) {
            Debug::log(ERR, "CANRManager::SANRData::runDialog: error spawning dialog");
            return;
        }

        const auto& result = r->result();

        if (result.starts_with("Terminate"))
            ::kill(dialogWmPID, SIGKILL);
        else if (result.starts_with("Wait"))
            dialogSaidWait = true;
        else
            Debug::log(ERR, "CANRManager::SANRData::runDialog: lambda: unrecognized result: {}", result);
    });
}

bool CANRManager::SANRData::isRunning() {
    return dialogBox && dialogBox->isRunning();
}

void CANRManager::SANRData::killDialog() {
    if (!dialogBox)
        return;

    dialogBox->kill();
    dialogBox = nullptr;
}

bool CANRManager::SANRData::fitsWindow(PHLWINDOW pWindow) const {
    if (pWindow->m_xwaylandSurface)
        return pWindow->m_xwaylandSurface == xwaylandSurface;
    else if (pWindow->m_xdgSurface)
        return pWindow->m_xdgSurface->m_owner == xdgBase && xdgBase;
    return false;
}

bool CANRManager::SANRData::isDefunct() const {
    return xdgBase.expired() && xwaylandSurface.expired();
}

pid_t CANRManager::SANRData::getPid() const {
    if (xdgBase) {
        pid_t pid = 0;
        wl_client_get_credentials(xdgBase->client(), &pid, nullptr, nullptr);
        return pid;
    }

    if (xwaylandSurface)
        return xwaylandSurface->m_pid;

    return cachedPid;
}

void CANRManager::SANRData::ping() {
    if (xdgBase) {
        xdgBase->ping();
        return;
    }

    if (xwaylandSurface)
        xwaylandSurface->ping();
}
