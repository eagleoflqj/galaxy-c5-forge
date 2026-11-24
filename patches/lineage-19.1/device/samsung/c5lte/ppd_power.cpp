// SPDX-License-Identifier: Apache-2.0
#include <android/os/BnPowerManager.h>
#include <binder/Parcel.h>
#include <binder/Status.h>

namespace android {

// Client ABI used by the CSA1 display daemon. The method order is part of its ABI.
class IPowerManager final : public IInterface {
public:
    static sp<IPowerManager> asInterface(const sp<IBinder>& binder);
    explicit IPowerManager(const sp<IBinder>& binder) : mBinder(binder) {}

    virtual const String16& getInterfaceDescriptor() const { return os::IPowerManager::descriptor; }

    virtual status_t acquireWakeLock(int flags, const sp<IBinder>& lock, const String16& tag,
                                     const String16& package, bool oneWay) {
        Parcel data;
        data.writeInterfaceToken(getInterfaceDescriptor());
        data.writeStrongBinder(lock);
        data.writeInt32(flags);
        data.writeString16(tag);
        data.writeString16(package);
        data.writeInt32(0);             // No WorkSource.
        data.writeString16(nullptr, 0); // No history tag.
        data.writeInt32(-1);            // Display.INVALID_DISPLAY, the default for a wake lock.
        return transact(os::BnPowerManager::TRANSACTION_acquireWakeLock, data, oneWay);
    }

    virtual status_t acquireWakeLockWithUid(int flags, const sp<IBinder>& lock, const String16& tag,
                                            const String16& package, int uid, bool oneWay) {
        Parcel data;
        data.writeInterfaceToken(getInterfaceDescriptor());
        data.writeStrongBinder(lock);
        data.writeInt32(flags);
        data.writeString16(tag);
        data.writeString16(package);
        data.writeInt32(uid);
        data.writeInt32(-1);
        return transact(os::BnPowerManager::TRANSACTION_acquireWakeLockWithUid, data, oneWay);
    }

    virtual status_t releaseWakeLock(const sp<IBinder>& lock, int flags, bool oneWay) {
        Parcel data;
        data.writeInterfaceToken(getInterfaceDescriptor());
        data.writeStrongBinder(lock);
        data.writeInt32(flags);
        return transact(os::BnPowerManager::TRANSACTION_releaseWakeLock, data, oneWay);
    }

private:
    IBinder* onAsBinder() override { return mBinder.get(); }

    status_t transact(uint32_t code, const Parcel& data, bool oneWay) {
        if (data.errorCheck() != OK) return data.errorCheck();
        Parcel reply;
        status_t rc = mBinder->transact(code, data, oneWay ? nullptr : &reply,
                                        oneWay ? IBinder::FLAG_ONEWAY : 0);
        if (rc != OK || oneWay) return rc;
        binder::Status result;
        rc = result.readFromParcel(reply);
        if (rc != OK) return rc;
        if (result.isOk()) return OK;
        return result.transactionError() != OK ? result.transactionError() : result.exceptionCode();
    }

    sp<IBinder> mBinder;
};

sp<IPowerManager> IPowerManager::asInterface(const sp<IBinder>& binder) {
    return binder != nullptr ? sp<IPowerManager>::make(binder) : nullptr;
}

} // namespace android
