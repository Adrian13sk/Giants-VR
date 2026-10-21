#include "xr/xr_input.h"

#include <cstring>
#include <vector>

namespace gvr {

XrPath XrInput::path(const char* s) {
    XrPath p = XR_NULL_PATH;
    XrResult r = xrStringToPath(instance_, s, &p);
    if (XR_FAILED(r)) LOGE("xrStringToPath(%s) failed: %d", s, static_cast<int>(r));
    return p;
}

bool XrInput::createAction(XrAction* out, XrActionType type, const char* name,
                           const char* localized, bool perHand) {
    XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
    ci.actionType = type;
    std::strncpy(ci.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(ci.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    if (perHand) {
        ci.countSubactionPaths = 2;
        ci.subactionPaths = handPath_;
    }
    XR_CHECK_RET(xrCreateAction(actionSet_, &ci, out), false);
    return true;
}

bool XrInput::init(XrInstance instance, XrSession session) {
    instance_ = instance;
    session_ = session;
    handPath_[kLeft] = path("/user/hand/left");
    handPath_[kRight] = path("/user/hand/right");

    XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(asci.actionSetName, "gameplay");
    std::strcpy(asci.localizedActionSetName, "Gameplay");
    asci.priority = 0;
    XR_CHECK_RET(xrCreateActionSet(instance_, &asci, &actionSet_), false);

    bool ok = true;
    ok &= createAction(&thumbstick_, XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick", "Thumbstick", true);
    ok &= createAction(&trigger_, XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "Trigger", true);
    ok &= createAction(&squeeze_, XR_ACTION_TYPE_FLOAT_INPUT, "squeeze", "Grip", true);
    ok &= createAction(&primary_, XR_ACTION_TYPE_BOOLEAN_INPUT, "primary", "Primary button (A/X)", true);
    ok &= createAction(&secondary_, XR_ACTION_TYPE_BOOLEAN_INPUT, "secondary", "Secondary button (B/Y)", true);
    ok &= createAction(&stickClick_, XR_ACTION_TYPE_BOOLEAN_INPUT, "stick_click", "Thumbstick click", true);
    ok &= createAction(&menu_, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Menu", true);
    ok &= createAction(&gripPose_, XR_ACTION_TYPE_POSE_INPUT, "grip_pose", "Grip pose", true);
    ok &= createAction(&aimPose_, XR_ACTION_TYPE_POSE_INPUT, "aim_pose", "Aim pose", true);
    ok &= createAction(&haptic_, XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", "Haptics", true);
    if (!ok) return false;

    // --- Oculus Touch bindings -------------------------------------------------------------
    {
        std::vector<XrActionSuggestedBinding> b = {
            {thumbstick_, path("/user/hand/left/input/thumbstick")},
            {thumbstick_, path("/user/hand/right/input/thumbstick")},
            {trigger_, path("/user/hand/left/input/trigger/value")},
            {trigger_, path("/user/hand/right/input/trigger/value")},
            {squeeze_, path("/user/hand/left/input/squeeze/value")},
            {squeeze_, path("/user/hand/right/input/squeeze/value")},
            {primary_, path("/user/hand/left/input/x/click")},
            {primary_, path("/user/hand/right/input/a/click")},
            {secondary_, path("/user/hand/left/input/y/click")},
            {secondary_, path("/user/hand/right/input/b/click")},
            {stickClick_, path("/user/hand/left/input/thumbstick/click")},
            {stickClick_, path("/user/hand/right/input/thumbstick/click")},
            {menu_, path("/user/hand/left/input/menu/click")},
            {gripPose_, path("/user/hand/left/input/grip/pose")},
            {gripPose_, path("/user/hand/right/input/grip/pose")},
            {aimPose_, path("/user/hand/left/input/aim/pose")},
            {aimPose_, path("/user/hand/right/input/aim/pose")},
            {haptic_, path("/user/hand/left/output/haptic")},
            {haptic_, path("/user/hand/right/output/haptic")},
        };
        XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        sb.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
        sb.suggestedBindings = b.data();
        sb.countSuggestedBindings = static_cast<uint32_t>(b.size());
        XR_CHECK_RET(xrSuggestInteractionProfileBindings(instance_, &sb), false);
    }
    // --- KHR simple controller fallback ----------------------------------------------------
    {
        std::vector<XrActionSuggestedBinding> b = {
            {trigger_, path("/user/hand/left/input/select/click")},
            {trigger_, path("/user/hand/right/input/select/click")},
            {menu_, path("/user/hand/left/input/menu/click")},
            {menu_, path("/user/hand/right/input/menu/click")},
            {gripPose_, path("/user/hand/left/input/grip/pose")},
            {gripPose_, path("/user/hand/right/input/grip/pose")},
            {aimPose_, path("/user/hand/left/input/aim/pose")},
            {aimPose_, path("/user/hand/right/input/aim/pose")},
            {haptic_, path("/user/hand/left/output/haptic")},
            {haptic_, path("/user/hand/right/output/haptic")},
        };
        XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        sb.interactionProfile = path("/interaction_profiles/khr/simple_controller");
        sb.suggestedBindings = b.data();
        sb.countSuggestedBindings = static_cast<uint32_t>(b.size());
        XR_LOG_FAIL(xrSuggestInteractionProfileBindings(instance_, &sb));
    }

    for (int h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo sci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        sci.subactionPath = handPath_[h];
        sci.poseInActionSpace.orientation.w = 1.0f;
        sci.action = gripPose_;
        XR_CHECK_RET(xrCreateActionSpace(session_, &sci, &gripSpace_[h]), false);
        sci.action = aimPose_;
        XR_CHECK_RET(xrCreateActionSpace(session_, &sci, &aimSpace_[h]), false);
    }

    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1;
    ai.actionSets = &actionSet_;
    XR_CHECK_RET(xrAttachSessionActionSets(session_, &ai), false);
    LOGI("XrInput: actions created and attached");
    return true;
}

float XrInput::getFloat(XrAction a, int hand) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    gi.subactionPath = handPath_[hand];
    XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
    if (XR_FAILED(xrGetActionStateFloat(session_, &gi, &st)) || !st.isActive) return 0.0f;
    return st.currentState;
}

bool XrInput::getBool(XrAction a, int hand) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    gi.subactionPath = handPath_[hand];
    XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_FAILED(xrGetActionStateBoolean(session_, &gi, &st)) || !st.isActive) return false;
    return st.currentState == XR_TRUE;
}

static Pose toPose(const XrPosef& p) {
    Pose r;
    r.orientation = {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
    r.position = {p.position.x, p.position.y, p.position.z};
    return r;
}

void XrInput::sync(XrTime time, XrSpace baseSpace, InputFrame& out) {
    XrActiveActionSet active{actionSet_, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &active;
    XrResult r = xrSyncActions(session_, &si);
    if (XR_FAILED(r)) {
        LOGW("xrSyncActions failed: %d", static_cast<int>(r));
        return;
    }
    // XR_SESSION_NOT_FOCUSED is a success code: all actions report inactive in that case.

    for (int h = 0; h < 2; ++h) {
        ControllerState& c = out.hand[h];
        c = ControllerState{};
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = thumbstick_;
        gi.subactionPath = handPath_[h];
        XrActionStateVector2f v2{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(xrGetActionStateVector2f(session_, &gi, &v2)) && v2.isActive)
            c.thumbstick = {v2.currentState.x, v2.currentState.y};
        c.trigger = getFloat(trigger_, h);
        c.squeeze = getFloat(squeeze_, h);
        c.primary = getBool(primary_, h);
        c.secondary = getBool(secondary_, h);
        c.thumbstickClick = getBool(stickClick_, h);
        c.menu = getBool(menu_, h);

        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        const XrSpaceLocationFlags need =
            XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if (XR_SUCCEEDED(xrLocateSpace(gripSpace_[h], baseSpace, time, &loc)) &&
            (loc.locationFlags & need) == need) {
            c.gripPose = toPose(loc.pose);
            c.gripPoseValid = true;
        }
        loc = {XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace(aimSpace_[h], baseSpace, time, &loc)) &&
            (loc.locationFlags & need) == need) {
            c.aimPose = toPose(loc.pose);
            c.aimPoseValid = true;
        }
        c.active = c.gripPoseValid || c.aimPoseValid;
    }
}

void XrInput::vibrate(int hand, float amplitude, float seconds, float frequencyHz) {
    if (!session_ || !haptic_) return;
    XrHapticVibration vib{XR_TYPE_HAPTIC_VIBRATION};
    vib.amplitude = amplitude;
    vib.duration = static_cast<XrDuration>(seconds * 1e9);
    vib.frequency = frequencyHz;
    XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
    hi.action = haptic_;
    hi.subactionPath = handPath_[hand];
    XR_LOG_FAIL(xrApplyHapticFeedback(session_, &hi,
                                      reinterpret_cast<const XrHapticBaseHeader*>(&vib)));
}

void XrInput::refreshInteractionProfiles() {
    for (int h = 0; h < 2; ++h) {
        XrInteractionProfileState st{XR_TYPE_INTERACTION_PROFILE_STATE};
        if (XR_FAILED(xrGetCurrentInteractionProfile(session_, handPath_[h], &st))) continue;
        if (st.interactionProfile == XR_NULL_PATH) {
            profile_[h] = "(none)";
            continue;
        }
        char buf[XR_MAX_PATH_LENGTH];
        uint32_t n = 0;
        if (XR_SUCCEEDED(xrPathToString(instance_, st.interactionProfile, sizeof(buf), &n, buf)))
            profile_[h] = buf;
        LOGI("Interaction profile %s hand: %s", h == 0 ? "left" : "right", profile_[h].c_str());
    }
}

void XrInput::destroy() {
    for (int h = 0; h < 2; ++h) {
        if (gripSpace_[h]) xrDestroySpace(gripSpace_[h]);
        if (aimSpace_[h]) xrDestroySpace(aimSpace_[h]);
        gripSpace_[h] = aimSpace_[h] = XR_NULL_HANDLE;
    }
    if (actionSet_) xrDestroyActionSet(actionSet_);  // destroys contained actions
    actionSet_ = XR_NULL_HANDLE;
}

}  // namespace gvr
