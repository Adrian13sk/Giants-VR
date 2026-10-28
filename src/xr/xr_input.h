// OpenXR action setup and per-frame polling for Quest Touch controllers.
#pragma once

#include <string>

#include "game/input_state.h"
#include "xr/xr_common.h"

namespace gvr {

class XrInput {
public:
    bool init(XrInstance instance, XrSession session);
    // Must be called once per frame after the session is running.
    void sync(XrTime time, XrSpace baseSpace, InputFrame& out);
    void vibrate(int hand, float amplitude, float seconds, float frequencyHz = XR_FREQUENCY_UNSPECIFIED);
    void refreshInteractionProfiles();
    void destroy();
    const std::string& profile(int hand) const { return profile_[hand]; }

private:
    XrInstance instance_ = XR_NULL_HANDLE;
    XrSession session_ = XR_NULL_HANDLE;
    XrActionSet actionSet_ = XR_NULL_HANDLE;
    XrPath handPath_[2] = {XR_NULL_PATH, XR_NULL_PATH};
    XrAction thumbstick_ = XR_NULL_HANDLE, trigger_ = XR_NULL_HANDLE, squeeze_ = XR_NULL_HANDLE;
    XrAction primary_ = XR_NULL_HANDLE, secondary_ = XR_NULL_HANDLE, stickClick_ = XR_NULL_HANDLE;
    XrAction menu_ = XR_NULL_HANDLE, gripPose_ = XR_NULL_HANDLE, aimPose_ = XR_NULL_HANDLE;
    XrAction haptic_ = XR_NULL_HANDLE;
    XrAction triggerTouch_ = XR_NULL_HANDLE, thumbTouch_ = XR_NULL_HANDLE;
    bool touchBound_ = false;
    XrSpace gripSpace_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    XrSpace aimSpace_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::string profile_[2];

    XrPath path(const char* s);
    bool createAction(XrAction* out, XrActionType type, const char* name, const char* localized,
                      bool perHand);
    float getFloat(XrAction a, int hand);
    bool getBool(XrAction a, int hand);
};

}  // namespace gvr
