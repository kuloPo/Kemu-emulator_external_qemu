/*
* Copyright (C) 2019 The Android Open Source Project
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include "aemu/base/logging/Log.h"
#include "host-common/MultiDisplayPipe.h"
#include "host-common/MultiDisplay.h"
#include "host-common/FeatureControl.h"
#include "host-common/Features.h"
#include "host-common/opengles.h"
#include "android/emulation/KemuTabPipe.h"
#include "android/console.h"
#include "android/emulation/control/globals_agent.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <mutex>

namespace {
std::mutex tabFramesMutex;
std::unordered_map<uint32_t, uint32_t> tabFrames;
constexpr uint8_t kTabFrame = 5;
}

namespace android {

static MultiDisplayPipe* sMultiDisplayPipeInstance = nullptr;
const uint8_t MultiDisplayPipe::ADD = 1;
const uint8_t MultiDisplayPipe::DEL = 2;
const uint8_t MultiDisplayPipe::QUERY = 3;
const uint8_t MultiDisplayPipe::BIND = 4;
const uint8_t MultiDisplayPipe::SET_DISPLAY = 0x10;
const uint8_t MultiDisplayPipe::MAX_DISPLAYS = 10;

MultiDisplayPipe::MultiDisplayPipe(AndroidPipe::Service* service, PipeArgs&& pipeArgs)
  : AndroidAsyncMessagePipe(service, std::move(pipeArgs)),
    mService(static_cast<Service*>(service)) {
    LOG(DEBUG) << "MultiDisplayPipe created " << this;
    if (sMultiDisplayPipeInstance) {
        derror("MultiDisplayPipe has already been created.");
    }
    sMultiDisplayPipeInstance = this;
}

MultiDisplayPipe::~MultiDisplayPipe() {
    LOG(DEBUG) << "MultiDisplayPipe deleted self " << this;
    sMultiDisplayPipeInstance = NULL;
    std::lock_guard<std::mutex> lock(tabFramesMutex);
    tabFrames.clear();
}

void MultiDisplayPipe::onMessage(const std::vector<uint8_t>& data) {
    if (data.empty()) return;
    uint8_t cmd = data[0];
    switch (cmd) {
        case QUERY: {
            LOG(DEBUG) << "MultiDisplayPipe recevied QUERY";
            uint32_t w, h, dpi, flag, id;
            int32_t startId = -1;
            const auto ins = MultiDisplay::getInstance();
            while (ins && ins->getNextMultiDisplay(startId, &id, nullptr, nullptr,
                                                    &w, &h, &dpi, &flag, nullptr)) {
                if (id >= MultiDisplay::s_displayIdInternalBegin) {
                    // Display created by guest through rcCommand, e.g., HWC2,
                    // not by UI/config.ini/cmd, need not report
                    break;
                }
                std::vector<uint8_t> buf;
                fillData(buf, id, w, h, dpi, flag, true);
                LOG(DEBUG) << "MultiDisplayPipe send add id " << id << " width " << w
                                << " height " << h << " dpi " << dpi << " flag " << flag;
                send(std::move(buf));
                startId = id;
            }
            break;
        }
        case kTabFrame: {
            if (data.size() != 9) return;
            uint32_t slot, buffer;
            std::memcpy(&slot, data.data() + 1, sizeof(slot));
            std::memcpy(&buffer, data.data() + 5, sizeof(buffer));
            {
                std::lock_guard<std::mutex> lock(tabFramesMutex);
                if (buffer) tabFrames[slot] = buffer;
                else tabFrames.erase(slot);
            }
            if (buffer && kemu::presentedTab.load(std::memory_order_relaxed) == int(slot)) {
                getConsoleAgents()->emu->paintMultiDisplayWindow(slot, buffer);
            }
            break;
        }
        case BIND: {
            uint32_t id = *(uint32_t*)&(data[1]);
            uint32_t cb = *(uint32_t*)&(data[5]);
            LOG(DEBUG) << "MultiDisplayPipe bind display " << id << " cb " << cb;
            const auto ins = MultiDisplay::getInstance();
            if (ins) {
                ins->setDisplayColorBuffer(id, cb);
            }
            break;
        }
        default:
            dwarning("Unexpected MultiDisplayPipe commmand: %d, ignoring. ", cmd);
    }

}

void MultiDisplayPipe::fillData(std::vector<uint8_t>& data, uint32_t id, uint32_t w, uint32_t h,
                                uint32_t dpi, uint32_t flag, bool add) {
    uint32_t input[] = {id, w, h, dpi, flag};
    uint8_t* p = (uint8_t*)input;
    if (add) {
        data.push_back(ADD);
    } else {
        data.push_back(DEL);
    }
    for (int i = 0; i < sizeof(input); i++) {
        data.push_back(p[i]);
    }
}

void MultiDisplayPipe::fillData(std::vector<uint8_t>& data,
                                uint32_t id,
                                uint32_t w,
                                uint32_t h,
                                uint32_t dpi,
                                uint32_t flag,
                                int mode) {
    uint32_t input[] = {id, w, h, dpi, flag};
    uint8_t* p = (uint8_t*)input;
    if (mode != SET_DISPLAY && mode != ADD && mode != DEL) {
        derror("invalid mode %d", mode);
        return;
    }
    data.push_back(mode);
    for (int i = 0; i < sizeof(input); i++) {
        data.push_back(p[i]);
    }
}

void MultiDisplayPipe::onSave(base::Stream* stream) {
    AndroidAsyncMessagePipe::onSave(stream);
    MultiDisplay* instance = MultiDisplay::getInstance();
    if (!instance) {
        derror("Failed to save MultiDisplay info, MultiDisplay not initialized");
        return;
    }
    instance->onSave(stream);
}

void MultiDisplayPipe::onLoad(base::Stream* stream) {
    AndroidAsyncMessagePipe::onLoad(stream);
    MultiDisplay* instance = MultiDisplay::getInstance();
    if (!instance) {
        derror("Failed to load MultiDisplay info, MultiDisplay not initialized");
        return;
    }
    instance->onLoad(stream);
}

MultiDisplayPipe* MultiDisplayPipe::getInstance() {
    return sMultiDisplayPipeInstance;
}
} // namespace android

namespace {
class TabPipe final : public android::AndroidAsyncMessagePipe {
public:
    using Service = android::AndroidAsyncMessagePipe::Service<TabPipe>;
    static Service* service;
    static std::mutex mutex;
    static android::AsyncMessagePipeHandle active;
    static std::optional<std::string> snapshot;
    static std::unordered_map<std::string, std::string> icons;

    TabPipe(android::AndroidPipe::Service* owner, PipeArgs&& args)
        : AndroidAsyncMessagePipe(owner, std::move(args)) {}
    ~TabPipe() override {
        std::lock_guard<std::mutex> lock(mutex);
        if (active == getHandle()) {
            active = {};
            snapshot = std::string();
            icons.clear();
        }
    }
    void onMessage(const std::vector<uint8_t>& data) override {
        if (data.empty()) return;
        std::lock_guard<std::mutex> lock(mutex);
        if (active.isValid() && !(active == getHandle())) return;
        active = getHandle();
        if (data.front() == 'I') {
            // I, NUL-terminated session token, PNG (empty on tab close).
            auto end = std::find(data.begin() + 1, data.end(), 0);
            if (end == data.end() || end == data.begin() + 1) return;
            std::string token(data.begin() + 1, end);
            if (++end == data.end()) {
                icons.erase(token);
            } else {
                icons[token].assign(end, data.end());
            }
            return;
        }
        snapshot.emplace(data.begin(), data.end());
    }
};
TabPipe::Service* TabPipe::service = nullptr;
std::mutex TabPipe::mutex;
android::AsyncMessagePipeHandle TabPipe::active;
std::optional<std::string> TabPipe::snapshot;
std::unordered_map<std::string, std::string> TabPipe::icons;
}

std::atomic<int> kemu::presentedTab{-1};
namespace {
std::mutex viewMutex;
gfxstream_display_view presentation{-1};
}

bool kemu::setPresentationView(const gfxstream_display_view& view) {
    std::lock_guard<std::mutex> lock(viewMutex);
    if (!memcmp(&view, &presentation, sizeof(view))) return false;
    presentation = view;
    presentedTab.store(view.slot, std::memory_order_relaxed);
    return true;
}

bool kemu::getPresentationView(gfxstream_display_view* view) {
    if (presentedTab.load(std::memory_order_relaxed) < 0) return false;
    std::lock_guard<std::mutex> lock(viewMutex);
    *view = presentation;
    if (view->slot > 0) {
        std::lock_guard<std::mutex> frames(tabFramesMutex);
        auto it = tabFrames.find(view->slot);
        view->colorBuffer = it == tabFrames.end() ? 0 : it->second;
    }
    return true;
}

std::optional<std::string> kemu::takeTabSnapshot() {
    std::lock_guard<std::mutex> lock(TabPipe::mutex);
    return std::exchange(TabPipe::snapshot, std::nullopt);
}

std::string kemu::tabIconPng(const std::string& token) {
    std::lock_guard<std::mutex> lock(TabPipe::mutex);
    auto it = TabPipe::icons.find(token);
    return it == TabPipe::icons.end() ? std::string() : it->second;
}

void kemu::sendTabCommand(const std::string& message) {
    android::AsyncMessagePipeHandle handle;
    {
        std::lock_guard<std::mutex> lock(TabPipe::mutex);
        handle = TabPipe::active;
    }
    if (!handle.isValid()) return;
    auto ref = TabPipe::service->getPipe(handle);
    if (ref) ref.value().pipe->send({message.begin(), message.end()});
}

bool kemu::appTabsEnabled() {
    const char* enabled = std::getenv("KEMU_APP_TABS_ACTIVE");
    return enabled && !std::strcmp(enabled, "1") &&
           android::featurecontrol::isEnabled(android::featurecontrol::Vulkan) &&
           android::featurecontrol::isEnabled(android::featurecontrol::VulkanNativeSwapchain);
}

void android_init_multi_display_pipe() {
    if (kemu::appTabsEnabled()) {
        auto service = std::make_unique<TabPipe::Service>("kemu-app-tabs-v2");
        TabPipe::service = service.get();
        android::AndroidPipe::Service::add(std::move(service));
    }
    android::AndroidPipe::Service::add(
        std::make_unique<android::MultiDisplayPipe::Service>("multidisplay"));
}

