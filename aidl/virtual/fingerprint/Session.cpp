/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Session.h"

#include <android-base/logging.h>

#include "util/CancellationSignal.h"

#undef LOG_TAG
#define LOG_TAG "FingerprintVirtualHalSession"

namespace aidl::android::hardware::biometrics::fingerprint {

void onClientDeath(void* cookie) {
    LOG(INFO) << "FingerprintService has died";
    Session* session = static_cast<Session*>(cookie);
    if (session && !session->isClosed()) {
        session->close();
    }
}

Session::Session(int sensorId, int userId, std::shared_ptr<ISessionCallback> cb,
                 FingerprintEngine* engine, WorkerThread* worker)
    : mSensorId(sensorId),
      mUserId(userId),
      mCb(std::move(cb)),
      mEngine(engine),
      mWorker(worker),
      mScheduledState(SessionState::IDLING),
      mCurrentState(SessionState::IDLING) {
    CHECK_GE(mSensorId, 0);
    CHECK_GE(mUserId, 0);
    CHECK(mEngine);
    CHECK(mWorker);
    CHECK(mCb);

    mDeathRecipient = AIBinder_DeathRecipient_new(onClientDeath);
}

binder_status_t Session::linkToDeath(AIBinder* binder) {
    return AIBinder_linkToDeath(binder, mDeathRecipient, this);
}

void Session::scheduleStateOrCrash(SessionState state) {
    // TODO(b/166800618): call enterIdling from the terminal callbacks and restore these checks.
    // CHECK(mScheduledState == SessionState::IDLING);
    // CHECK(mCurrentState == SessionState::IDLING);
    mScheduledState = state;
}

void Session::enterStateOrCrash(SessionState state) {
    CHECK(mScheduledState == state);
    mCurrentState = state;
    mScheduledState = SessionState::IDLING;
}

void Session::enterIdling() {
    // TODO(b/166800618): call enterIdling from the terminal callbacks and rethink this conditional.
    if (mCurrentState != SessionState::CLOSED) {
        mCurrentState = SessionState::IDLING;
    }
}

bool Session::isClosed() {
    return mCurrentState == SessionState::CLOSED;
}

ndk::ScopedAStatus Session::generateChallenge() {
    LOG(INFO) << "generateChallenge";
    scheduleStateOrCrash(SessionState::GENERATING_CHALLENGE);

    mWorker->schedule(Callable::from([this] {
        enterStateOrCrash(SessionState::GENERATING_CHALLENGE);
        mEngine->generateChallengeImpl(mCb.get());
        enterIdling();
    }));

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::revokeChallenge(int64_t challenge) {
    LOG(INFO) << "revokeChallenge";
    scheduleStateOrCrash(SessionState::REVOKING_CHALLENGE);

    mWorker->schedule(Callable::from([this, challenge] {
        enterStateOrCrash(SessionState::REVOKING_CHALLENGE);
        mEngine->revokeChallengeImpl(mCb.get(), challenge);
        enterIdling();
    }));

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::enroll(const keymaster::HardwareAuthToken& hat,
                                   std::shared_ptr<common::ICancellationSignal>* out) {
    LOG(INFO) << "enroll";
    scheduleStateOrCrash(SessionState::ENROLLING);

    std::promise<void> cancellationPromise;
    auto cancFuture = cancellationPromise.get_future();

    mWorker->schedule(Callable::from([this, hat, cancFuture = std::move(cancFuture)] {
        enterStateOrCrash(SessionState::ENROLLING);
        if (shouldCancel(cancFuture)) {
            mCb->onError(Error::CANCELED, 0 /* vendorCode */);
        } else {
            mEngine->enrollImpl(mCb.get(), hat, cancFuture);
        }
        enterIdling();
    }));

    *out = SharedRefBase::make<CancellationSignal>(std::move(cancellationPromise));
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::authenticate(int64_t operationId,
                                         std::shared_ptr<common::ICancellationSignal>* out) {
    LOG(INFO) << "authenticate";
    scheduleStateOrCrash(SessionState::AUTHENTICATING);

    std::promise<void> cancPromise;
    auto cancFuture = cancPromise.get_future();

    mWorker->schedule(Callable::from([this, operationId, cancFuture = std::move(cancFuture)] {
        enterStateOrCrash(SessionState::AUTHENTICATING);
        if (shouldCancel(cancFuture)) {
            mCb->onError(Error::CANCELED, 0 /* vendorCode */);
        } else {
            mEngine->authenticateImpl(mCb.get(), operationId, cancFuture);
        }
        enterIdling();
    }));

    *out = SharedRefBase::make<CancellationSignal>(std::move(cancPromise));
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::detectInteraction(std::shared_ptr<common::ICancellationSignal>* out) {
    LOG(INFO) << "detectInteraction";
    scheduleStateOrCrash(SessionState::DETECTING_INTERACTION);

    std::promise<void> cancellationPromise;
    auto cancFuture = cancellationPromise.get_future();

    mWorker->schedule(Callable::from([this, cancFuture = std::move(cancFuture)] {
        enterStateOrCrash(SessionState::DETECTING_INTERACTION);
        if (shouldCancel(cancFuture)) {
            mCb->onError(Error::CANCELED, 0 /* vendorCode */);
        } else {
            mEngine->detectInteractionImpl(mCb.get(), cancFuture);
        }
        enterIdling();
    }));

    *out = SharedRefBase::make<CancellationSignal>(std::move(cancellationPromise));
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::enumerateEnrollments() {
    LOG(INFO) << "enumerateEnrollments";
    scheduleStateOrCrash(SessionState::ENUMERATING_ENROLLMENTS);

    mWorker->schedule(Callable::from([this] {
        enterStateOrCrash(SessionState::ENUMERATING_ENROLLMENTS);
        mEngine->enumerateEnrollmentsImpl(mCb.get());
        enterIdling();
    }));

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::removeEnrollments(const std::vector<int32_t>& enrollmentIds) {
    LOG(INFO) << "removeEnrollments, size:" << enrollmentIds.size();
    scheduleStateOrCrash(SessionState::REMOVING_ENROLLMENTS);

    mWorker->schedule(Callable::from([this, enrollmentIds] {
        enterStateOrCrash(SessionState::REMOVING_ENROLLMENTS);
        mEngine->removeEnrollmentsImpl(mCb.get(), enrollmentIds);
        enterIdling();
    }));

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::getAuthenticatorId() {
    LOG(INFO) << "getAuthenticatorId";
    scheduleStateOrCrash(SessionState::GETTING_AUTHENTICATOR_ID);

    mWorker->schedule(Callable::from([this] {
        enterStateOrCrash(SessionState::GETTING_AUTHENTICATOR_ID);
        mEngine->getAuthenticatorIdImpl(mCb.get());
        enterIdling();
    }));

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::invalidateAuthenticatorId() {
    LOG(INFO) << "invalidateAuthenticatorId";
    scheduleStateOrCrash(SessionState::INVALIDATING_AUTHENTICATOR_ID);

    mWorker->schedule(Callable::from([this] {
        enterStateOrCrash(SessionState::INVALIDATING_AUTHENTICATOR_ID);
        mEngine->invalidateAuthenticatorIdImpl(mCb.get());
        enterIdling();
    }));

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::resetLockout(const keymaster::HardwareAuthToken& hat) {
    LOG(INFO) << "resetLockout";
    scheduleStateOrCrash(SessionState::RESETTING_LOCKOUT);

    mWorker->schedule(Callable::from([this, hat] {
        enterStateOrCrash(SessionState::RESETTING_LOCKOUT);
        mEngine->resetLockoutImpl(mCb.get(), hat);
        enterIdling();
    }));

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::close() {
    LOG(INFO) << "close";
    // TODO(b/166800618): call enterIdling from the terminal callbacks and restore this check.
    // CHECK(mCurrentState == SessionState::IDLING) << "Can't close a non-idling session.
    // Crashing.";
    mCurrentState = SessionState::CLOSED;
    mCb->onSessionClosed();
    AIBinder_DeathRecipient_delete(mDeathRecipient);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::onPointerDown(int32_t pointerId, int32_t x, int32_t y, float minor,
                                          float major) {
    LOG(INFO) << "onPointerDown";
    mEngine->notifyFingerdown();
    mWorker->schedule(Callable::from([this, pointerId, x, y, minor, major] {
        mEngine->onPointerDownImpl(pointerId, x, y, minor, major);
        enterIdling();
    }));
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::onPointerUp(int32_t pointerId) {
    LOG(INFO) << "onPointerUp";
    mWorker->schedule(Callable::from([this, pointerId] {
        mEngine->onPointerUpImpl(pointerId);
        enterIdling();
    }));
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::onUiReady() {
    LOG(INFO) << "onUiReady";
    mWorker->schedule(Callable::from([this] {
        mEngine->onUiReadyImpl();
        enterIdling();
    }));
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::authenticateWithContext(
        int64_t operationId, const common::OperationContext& /*context*/,
        std::shared_ptr<common::ICancellationSignal>* out) {
    return authenticate(operationId, out);
}

ndk::ScopedAStatus Session::enrollWithContext(const keymaster::HardwareAuthToken& hat,
                                              const common::OperationContext& /*context*/,
                                              std::shared_ptr<common::ICancellationSignal>* out) {
    return enroll(hat, out);
}

ndk::ScopedAStatus Session::detectInteractionWithContext(
        const common::OperationContext& /*context*/,
        std::shared_ptr<common::ICancellationSignal>* out) {
    return detectInteraction(out);
}

ndk::ScopedAStatus Session::onPointerDownWithContext(const PointerContext& context) {
    return onPointerDown(context.pointerId, context.x, context.y, context.minor, context.major);
}

ndk::ScopedAStatus Session::onPointerUpWithContext(const PointerContext& context) {
    return onPointerUp(context.pointerId);
}

ndk::ScopedAStatus Session::onContextChanged(const common::OperationContext& /*context*/) {
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::onPointerCancelWithContext(const PointerContext& /*context*/) {
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::setIgnoreDisplayTouches(bool /*shouldIgnore*/) {
    return ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::biometrics::fingerprint
