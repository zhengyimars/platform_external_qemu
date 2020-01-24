// Copyright (C) 2019 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "android/snapshot/Icebox.h"

#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <stdio.h>
#include <atomic>
#include <memory>
#include <vector>

#include "android/base/async/ThreadLooper.h"
#include "android/base/files/PathUtils.h"
#include "android/base/sockets/ScopedSocket.h"
#include "android/base/sockets/SocketUtils.h"
#include "android/base/synchronization/ConditionVariable.h"
#include "android/base/synchronization/Lock.h"
#include "android/base/system/System.h"
#include "android/base/threads/Async.h"
#include "android/base/threads/FunctorThread.h"
#include "android/base/threads/Thread.h"
#include "android/emulation/apacket_utils.h"
#include "android/emulation/AdbMessageSniffer.h"
#include "android/emulation/control/vm_operations.h"
#include "android/jdwp/Jdwp.h"
#include "android/snapshot/interface.h"

#define DEBUG 0

#if DEBUG >= 1
#define D(...) fprintf(stderr, __VA_ARGS__), fprintf(stderr, "\n")
#else
#define D(...) (void)0
#endif

#if DEBUG >= 2
#define DD(...) fprintf(stderr, __VA_ARGS__), fprintf(stderr, "\n")
#else
#define DD(...) (void)0
#endif

using amessage = android::emulation::AdbMessageSniffer::amessage;
using namespace android::emulation;
using namespace android::jdwp;

static int s_adb_port = -1;
static int s_adb_scoket = -1;
static std::atomic<std::uint32_t> s_id = {6000};
static std::unique_ptr<android::base::Thread> s_workerThread = {};

// Adb authentication
#define TOKEN_SIZE 20

static bool read_key(const char* file, std::vector<RSA*>& keys) {
    FILE* fp = fopen(file, "r");
    if (!fp) {
        DD("Failed to open '%s': %s", file, strerror(errno));
        return false;
    }

    RSA* key_rsa = RSA_new();

    if (!PEM_read_RSAPrivateKey(fp, &key_rsa, NULL, NULL)) {
        DD("Failed to read key");
        fclose(fp);
        RSA_free(key_rsa);
        return false;
    }

    keys.push_back(key_rsa);
    fclose(fp);
    return true;
}

static bool sign_token(RSA* key_rsa,
                       const char* token,
                       int token_size,
                       char* sig,
                       int& len) {
    if (token_size != TOKEN_SIZE) {
        DD("Unexpected token size %d\n", token_size);
    }

    if (!RSA_sign(NID_sha1, (const unsigned char*)token, (size_t)token_size,
                  (unsigned char*)sig, (unsigned int*)&len, key_rsa)) {
        return false;
    }

    DD("successfully signed with siglen %d\n", (int)len);
    return true;
}

static bool sign_auth_token(const char* token,
                            int token_size,
                            char* sig,
                            int& siglen) {
    // read key
    using android::base::pj;
    std::vector<RSA*> keys;
    std::string key_path = pj(android::base::System::get()->getHomeDirectory(),
                              ".android", "adbkey");
    if (!read_key(key_path.c_str(), keys)) {
        // TODO: test the windows code path
        std::string key_path =
                pj(android::base::System::get()->getAppDataDirectory(),
                   ".android", "adbkey");
        if (!read_key(key_path.c_str(), keys)) {
            return false;
        }
    }
    return sign_token(keys[0], token, token_size, sig, siglen);
}

static int tryConnect() {
    using namespace android::base;
    if (s_adb_scoket > 0) {
        return s_adb_scoket;
    }
    if (s_adb_port == -1) {
        fprintf(stderr, "adb port uninitialized\n");
        return false;
    }

    int s = socketTcp4LoopbackClient(s_adb_port);
    if (s < 0) {
        s = socketTcp6LoopbackClient(s_adb_port);
    }
    if (s < 0) {
        fprintf(stderr, "failed to connect to adb port %d\n", s_adb_port);
        return -1;
    }

#define _SEND_PACKET(packet)           \
    {                                  \
        if (!sendPacket(s, &packet)) { \
            return -1;                 \
        }                              \
    }
#define _RECV_PACKET(packet)           \
    {                                  \
        if (!recvPacket(s, &packet)) { \
            return -1;                 \
        }                              \
    }

    uint32_t local_id = s_id.fetch_add(1);
    uint32_t remote_id = 0;
    // It is ok for some non-thread safe to happen during the following loop,
    // as long as you don't have 2^32 threads simultaneously.
    if (s_id == 0) {
        s_id++;
    }
    socketSetBlocking(s);
    socketSetNoDelay(s);
    D("Setup socket");

    {
        apacket to_guest;
        const char* kCnxnData = "";
        to_guest.mesg.command = ADB_CNXN;
        to_guest.mesg.arg1 = 256 * 1024;
        to_guest.mesg.data_length = (unsigned)strlen(kCnxnData) + 1;
        to_guest.mesg.magic = ADB_CNXN ^ 0xffffffff;

        to_guest.data.resize(strlen(kCnxnData) + 1);
        memcpy(to_guest.data.data(), kCnxnData, strlen(kCnxnData));
        to_guest.data[strlen(kCnxnData)] = 0;
        DD("now write connection command...\n");
        _SEND_PACKET(to_guest);

        DD("now read ...\n");
        apacket pack_recv;
        _RECV_PACKET(pack_recv);

        // Authenticate ADB for playstore images
        while (pack_recv.mesg.command == ADB_AUTH) {
            int sigLen = 256;
            apacket pack_send;
            pack_send.mesg.command = ADB_AUTH;
            pack_send.mesg.arg0 = ADB_AUTH_SIGNATURE;
            pack_send.mesg.data_length = (unsigned)sigLen;
            pack_send.mesg.magic = ADB_AUTH ^ 0xffffffff;

            pack_send.data.resize(sigLen);
            if (!sign_auth_token((const char*)pack_recv.data.data(),
                                 pack_recv.mesg.data_length,
                                 (char*)pack_send.data.data(), sigLen)) {
                fprintf(stderr, "Fail to authenticate adb\n");
                return false;
            }
            assert(sigLen <= pack_send.data.size());
            pack_send.mesg.data_length = sigLen;
            pack_send.data.resize(sigLen);

            DD("send auth packet\n");
            _SEND_PACKET(pack_send);

            DD("read for connection\n");
            _RECV_PACKET(pack_recv);
        }
    }
    s_adb_scoket = s;
#undef _SEND_PACKET
#undef _RECV_PACKET
    return s;
}

static bool recvPacketWithId(int s, int hostId, apacket* packet) {
    do {
        if (!recvPacket(s, packet)) {
            return false;
        }
    } while (packet->mesg.arg1 != hostId);
    return true;
}

static bool recvOkayWithId(int s, int hostId) {
    apacket packet;
    if (!recvPacketWithId(s, hostId, &packet)) {
        return false;
    }
    return packet.mesg.command == ADB_OKAY;
}

namespace android {
namespace icebox {

void set_jdwp_port(int adb_port) {
    s_adb_port = adb_port;
}

bool run_async(const char* cmd) {
    if (s_workerThread) {
        if (!s_workerThread->tryWait(nullptr)) {
            return false;
        }
    }
    std::string cmd_str(cmd);
    s_workerThread.reset(new android::base::FunctorThread([cmd_str] {
        int s = tryConnect();
        if (s < 0) {
            return -1;
        }
        uint32_t local_id = s_id.fetch_add(1);
        uint32_t remote_id = 0;
#define _SEND_PACKET(packet)           \
    {                                  \
        if (!sendPacket(s, &packet)) { \
            return -1;                 \
        }                              \
    }
#define _RECV_PACKET(packet)                           \
    {                                                  \
        if (!recvPacketWithId(s, local_id, &packet)) { \
            return -1;                                 \
        }                                              \
    }
        {
            apacket connect;
            connect.mesg.command = ADB_OPEN;
            connect.mesg.arg0 = local_id;
            connect.mesg.data_length = (unsigned)cmd_str.length() + 1;
            connect.mesg.magic =  ADB_OPEN ^ 0xffffffff;

            connect.data.resize(connect.mesg.data_length);
            memcpy(connect.data.data(), cmd_str.data(), cmd_str.length());
            connect.data[cmd_str.length()] = 0;
            _SEND_PACKET(connect);
        }
        {
            apacket connect_ok;
            _RECV_PACKET(connect_ok);
            if (connect_ok.mesg.command != ADB_OKAY) {
                return -1;
            }
            remote_id = connect_ok.mesg.arg0;
            local_id = connect_ok.mesg.arg1;
        }
        return 0;
#undef _SEND_PACKET
#undef _RECV_PACKET
    }));
    return s_workerThread->start();
}

bool track_async(int pid, const char* snapshot_name) {
    if (s_workerThread) {
        if (!s_workerThread->tryWait(nullptr)) {
            return false;
        }
    }
    s_workerThread.reset(new android::base::FunctorThread([pid, snapshot_name] {
        bool result = track(pid, snapshot_name);
        D("track result %d\n", result);
    }));
    return s_workerThread->start();
}
bool track(int pid, const char* snapshot_name) {
    if (s_adb_port == -1) {
        fprintf(stderr, "adb port uninitialized\n");
        return false;
    }

    uint32_t local_id = s_id.fetch_add(1);
    uint32_t remote_id = 0;
    // It is ok for some non-thread safe to happen during the following loop,
    // as long as you don't have 2^32 threads simultaneously.
    if (s_id == 0) {
        s_id++;
    }
    D("Setup socket");
    int s = tryConnect();
    if (s < 0) {
        return false;
    }

#define _SEND_PACKET(packet)           \
    {                                  \
        if (!sendPacket(s, &packet)) { \
            return false;              \
        }                              \
    }
#define _RECV_PACKET(packet)                           \
    {                                                  \
        if (!recvPacketWithId(s, local_id, &packet)) { \
            return false;                              \
        }                                              \
    }
#define _SEND_PACKET_OK(packet)             \
    {                                       \
        _SEND_PACKET(packet);               \
        if (!recvOkayWithId(s, local_id)) { \
            return false;                   \
        }                                   \
    }
    {
        apacket jdwp_connect;
        jdwp_connect.mesg.command = ADB_OPEN;
        jdwp_connect.mesg.arg0 = local_id;
        jdwp_connect.mesg.magic = ADB_OPEN ^ 0xffffffff;

        jdwp_connect.data.resize(20);
        jdwp_connect.mesg.data_length =
                sprintf((char*)jdwp_connect.data.data(), "jdwp:%d", pid) + 1;
        jdwp_connect.data.resize(jdwp_connect.mesg.data_length);
        _SEND_PACKET(jdwp_connect);
    }
    {
        apacket connect_ok;
        _RECV_PACKET(connect_ok);
        if (connect_ok.mesg.command != ADB_OKAY) {
            return false;
        }
        remote_id = connect_ok.mesg.arg0;
        local_id = connect_ok.mesg.arg1;  // Hack replace host ID
    }
    D("Open jdwp");
    apacket ok_out;
    ok_out.mesg.command = ADB_OKAY;
    ok_out.mesg.arg0 = local_id;
    ok_out.mesg.arg1 = remote_id;
    ok_out.mesg.magic = ADB_OKAY ^ 0xffffffff;

    ok_out.data.clear();
#define _RECV_PACKET_OK(packet) \
    {                           \
        _RECV_PACKET(packet);   \
        _SEND_PACKET(ok_out);   \
    }

    apacket packet_out;
    packet_out.mesg.command = ADB_WRTE;
    packet_out.mesg.arg0 = local_id;
    packet_out.mesg.arg1 = remote_id;
    packet_out.mesg.magic = ADB_WRTE ^ 0xffffffff;

    // Send handshake
    packet_out.mesg.data_length = 14;
    packet_out.data.resize(14);
    // Don't use strcpy. We don't need trailing 0.
    memcpy(packet_out.data.data(), "JDWP-Handshake", 14);
    _SEND_PACKET_OK(packet_out);
    D("Handshake sent OK");
    {
        apacket handshake_recv;
        _RECV_PACKET_OK(handshake_recv);
        D("Handshake recv OK");
        if (memcmp(handshake_recv.data.data(), "JDWP-Handshake", 14) != 0) {
            return false;
        }
    }
    D("Handshake OK");
    uint32_t jdwp_id = 1;
    JdwpIdSize id_size;
    const char* kExceptionClass = "Ljava/lang/AssertionError;";
    std::vector<uint64_t> exception_reference_type_ids(0);
    {
        // ID size
        apacket reply;
        JdwpCommandHeader jdwp_query = {
                11 /*length*/,
                jdwp_id++ /*id*/,
                0 /*flags*/,
                CommandSet::VirtualMachine /*command_set*/,
                VirtualMachineCommand::IDSizes /*command*/,
        };
        packet_out.mesg.data_length = jdwp_query.length;
        packet_out.data.resize(jdwp_query.length);
        jdwp_query.writeToBuffer(packet_out.data.data());
        _SEND_PACKET_OK(packet_out);
        D("ID size query OK");
        _RECV_PACKET_OK(reply);
        id_size.parseFrom(reply.data.data() + 11);

        // Version
        jdwp_query.command = VirtualMachineCommand::Version;
        jdwp_query.writeToBuffer(packet_out.data.data());
        _SEND_PACKET_OK(packet_out);
        _RECV_PACKET_OK(reply);

        // Capatibility
        jdwp_query.command = VirtualMachineCommand::Capabilities;
        jdwp_query.writeToBuffer(packet_out.data.data());
        _SEND_PACKET_OK(packet_out);
        _RECV_PACKET_OK(reply);

        // Class by signature
        {
            jdwp_query.command = VirtualMachineCommand::ClassBySignature;
            jdwp_query.length = 11 + 4 + strlen(kExceptionClass);
            packet_out.mesg.data_length = jdwp_query.length;
            packet_out.data.resize(packet_out.mesg.data_length);
            uint8_t* data_end = writeStrToBuffer(packet_out.data.data() + 11,
                                                 kExceptionClass);
            assert(jdwp_query.length == data_end - packet_out.data.data());
            packet_out.mesg.data_length = jdwp_query.length;
            packet_out.data.resize(packet_out.mesg.data_length);
            jdwp_query.writeToBuffer(packet_out.data.data());
            _SEND_PACKET_OK(packet_out);
            _RECV_PACKET_OK(reply);
            uint8_t* data_ptr = reply.data.data() + 11;
            exception_reference_type_ids.resize(uint32FromBuffer(data_ptr));
            data_ptr += 4;
            for (auto& id : exception_reference_type_ids) {
                data_ptr += 1;
                id = readValFromBuffer<uint64_t>(data_ptr,
                                                 id_size.reference_typ_id_size);
                data_ptr += id_size.reference_typ_id_size;
                data_ptr += 4;
                D("%s: 0x%x", kExceptionClass, (int)id);
            }
        }

#if DEBUG >= 1
        {
            // Query all classes and validate exception class id
            jdwp_query.command = VirtualMachineCommand::AllClasses;
            jdwp_query.length = 11;
            packet_out.mesg.data_length = jdwp_query.length;
            packet_out.data.resize(packet_out.mesg.data_length);
            jdwp_query.writeToBuffer(packet_out.data.data());
            _SEND_PACKET_OK(packet_out);
            _RECV_PACKET_OK(reply);
            JdwpCommandHeader class_header;
            class_header.parseFrom(reply.data.data());
            std::vector<uint8_t> class_buffer(class_header.length);
            int total_length = 0;
            do {
                int header_bytes = total_length == 0 ? 11 : 0;
                memcpy(class_buffer.data() + total_length,
                       reply.data.data() + header_bytes,
                       reply.mesg.data_length - header_bytes);
                total_length += reply.mesg.data_length - header_bytes;
                if (total_length < class_header.length - 11) {
                    _RECV_PACKET_OK(reply);
                } else {
                    break;
                }
            } while (true);

            JdwpAllClasses classes;
            classes.parseFrom(class_buffer.data(), id_size);
            for (const auto& clazz : classes.classes) {
                DD("class %s id 0x%x\n", clazz.signature.c_str(),
                   (uint32_t)clazz.type_id);
                if (clazz.signature == kExceptionClass) {
                    bool found = false;
                    for (const auto& id : exception_reference_type_ids) {
                        if (id == clazz.type_id) {
                            found = true;
                            break;
                        }
                    }
                    assert(found);
                }
            }
        }
#endif
    }

    {
        // The first several commands have the same size
        const int kInitBufferSize = 200;
        apacket reply;
        JdwpCommandHeader jdwp_header;
        jdwp_header.id = jdwp_id++;
        jdwp_header.flags = 0;
        jdwp_header.command_set = CommandSet::EventRequest;
        jdwp_header.command = EventRequestCommand::Set;

        JdwpEventRequestSet set_request;
        set_request.event_kind = EventKind::Exception;
        set_request.suspend_policy = SuspendPolicy::All;
        for (const auto& id : exception_reference_type_ids) {
            packet_out.data.resize(kInitBufferSize);
            jdwp_header.length =
                    11 + set_request.writeToBuffer(packet_out.data.data() + 11,
                                                   id, &id_size);
            jdwp_header.id = jdwp_id++;
            jdwp_header.writeToBuffer(packet_out.data.data());
            CHECK(kInitBufferSize >= jdwp_header.length);
            packet_out.mesg.data_length = jdwp_header.length;
            packet_out.data.resize(packet_out.mesg.data_length);
            _SEND_PACKET_OK(packet_out);
            _RECV_PACKET_OK(reply);
        }
    }
    while (1) {
        apacket reply;
        _RECV_PACKET(reply);
        if (reply.mesg.command == ADB_CLSE) {
            return true;
        }
        // we wait for the snapshot before replying OK, to avoid any concurrency
        // issue between pipe receive and snapshots.
        if (reply.mesg.data_length > 11 &&
            reply.data[11] == SuspendPolicy::All) {  // Thread suspend all
            // Take snapshot when AssertionError is thrown
            bool snapshot_done = false;
            base::ConditionVariable snapshot_signal;
            base::Lock snapshot_lock;
            D("send out command for main thread");
            android::base::ThreadLooper::runOnMainLooper(
                    [&snapshot_done, &snapshot_signal, &snapshot_lock]() {
                        D("ready to take snapshot");
                        const AndroidSnapshotStatus result =
                                androidSnapshot_save("test_failure_snapshot");
                        D("Snapshot done, result %d", result);
                        snapshot_lock.lock();
                        snapshot_done = true;
                        snapshot_signal.broadcastAndUnlock(&snapshot_lock);
                    });
            snapshot_lock.lock();
            snapshot_signal.wait(&snapshot_lock,
                                 [&snapshot_done]() { return snapshot_done; });
            snapshot_lock.unlock();
            _SEND_PACKET(ok_out);
            // Resume and quit
            JdwpCommandHeader jdwp_command = {
                    11 /*length*/,
                    jdwp_id++ /*id*/,
                    0 /*flags*/,
                    CommandSet::VirtualMachine /*command_set*/,
                    VirtualMachineCommand::Resume /*command*/,
            };
            packet_out.mesg.data_length = jdwp_command.length;
            packet_out.data.resize(packet_out.mesg.data_length);
            jdwp_command.writeToBuffer(packet_out.data.data());
            _SEND_PACKET_OK(packet_out);
            _RECV_PACKET_OK(reply);
            packet_out.mesg.command = ADB_CLSE;
            packet_out.mesg.data_length = 0;
            packet_out.data.resize(0);
            packet_out.mesg.magic = packet_out.mesg.command ^ 0xffffffff;
            _SEND_PACKET(packet_out);
            //_RECV_PACKET(reply);
            return true;
        }
        _SEND_PACKET(ok_out);
    }
    return true;
#undef _SEND_PACKET
#undef _RECV_PACKET
#undef _SEND_PACKET_OK
#undef _RECV_PACKET_OK
}
}  // namespace icebox
}  // namespace android