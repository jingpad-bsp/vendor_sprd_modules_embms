/**
 * embms.cpp ---
 *
 *
 */

#define LOG_TAG "EMBMS"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <utils/Log.h>
#include <signal.h>
#include <sys/prctl.h>
#include <errno.h>
#include <private/android_filesystem_config.h>
#include <sys/capability.h>
#include <pwd.h>
#include <cutils/properties.h>
#include <cutils/sockets.h>
#include <getopt.h>
#include "embms.h"

#include <vendor/sprd/hardware/radio/lite/1.0/ILiteRadio.h>

using namespace vendor::sprd::hardware::radio::lite::V1_0;
using ::android::hardware::hidl_string;
using namespace std;
using ::android::hardware::Return;
using ::android::hardware::Void;
using android::sp;

#define RADIO1_SERVICE_NAME         "liteservice1"
#define RADIO2_SERVICE_NAME         "liteservice2"

#define MAX_CAP_NUM         (CAP_TO_INDEX(CAP_LAST_CAP) + 1)
#define MAX_Lite_COUNT              2

int s_fdListen[MAX_SERVICE_NUM];
int s_fdClient[MAX_SERVICE_NUM];
const char *s_serviceName[MAX_SERVICE_NUM] = {
    "EMBMS",
    "MDT"
};

const SERVICE_ID s_funcId[MAX_SERVICE_NUM] = {
    EMBMS,
    MDT
};
const char *s_socketName[MAX_SERVICE_NUM] = {
    "embmsd",
    "mdt_socket"
};

const char *s_processName[MAX_SERVICE_NUM] = {
    "u0_a84",
    "mdt"
};

ChannelInfo s_channelInfo[MAX_SERVICE_NUM][MUX_NUM];
ChannelInfo s_socketInfo[MAX_SERVICE_NUM];

static pthread_mutex_t  s_mainwriteMutex[MAX_SERVICE_NUM];

sp<ILiteRadio> s_liteRadioProxy[MAX_Lite_COUNT] = {
        NULL
#if (MAX_Lite_COUNT >= 2)
        , NULL
#endif
#if (MAX_Lite_COUNT >= 3)
        , NULL
#endif
#if (MAX_Lite_COUNT >= 4)
        , NULL
#endif
};

sp<ILiteRadioResponse> s_liteRadioResponse;
sp<ILiteRadioIndication> s_liteRadioIndication;

void switchUser() {
    prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0);
    if (setresuid(AID_SYSTEM, AID_SYSTEM, AID_SYSTEM) == -1) {
        EMBMS_LOGE("setresuid failed: %s", strerror(errno));
    }
    struct __user_cap_header_struct header;
    memset(&header, 0, sizeof(header));
    header.version = _LINUX_CAPABILITY_VERSION_3;
    header.pid = 0;

    struct __user_cap_data_struct data[MAX_CAP_NUM];
    memset(&data, 0, sizeof(data));

    data[CAP_TO_INDEX(CAP_NET_ADMIN)].effective |= CAP_TO_MASK(CAP_NET_ADMIN);
    data[CAP_TO_INDEX(CAP_NET_ADMIN)].permitted |= CAP_TO_MASK(CAP_NET_ADMIN);

    data[CAP_TO_INDEX(CAP_NET_RAW)].effective |= CAP_TO_MASK(CAP_NET_RAW);
    data[CAP_TO_INDEX(CAP_NET_RAW)].permitted |= CAP_TO_MASK(CAP_NET_RAW);

    data[CAP_TO_INDEX(CAP_BLOCK_SUSPEND)].effective |= CAP_TO_MASK(CAP_BLOCK_SUSPEND);
    data[CAP_TO_INDEX(CAP_BLOCK_SUSPEND)].permitted |= CAP_TO_MASK(CAP_BLOCK_SUSPEND);

    if (capset(&header, &data[0]) == -1) {
        EMBMS_LOGE("capset failed: %s", strerror(errno));
    }
}

static bool copyHidlStringToCharPtr(char **dest, const hidl_string &src) {
    size_t len = src.size();
    if (len == 0) {
        EMBMS_LOGD("copyHidlStringToCharPtr src len is 0");
        *dest = NULL;
        return true;
    }
    *dest = (char *) calloc(len + 1, sizeof(char));
    if (*dest == NULL) {
        EMBMS_LOGE("copyHidlStringToCharPtr Memory allocation failed");
        return false;
    }
    strncpy(*dest, src.c_str(), len + 1);
    return true;
}

static hidl_string convertCharPtrToHidlString(const char *ptr) {
    hidl_string ret;
    if (ptr != NULL) {
        // TODO: replace this with strnlen
        ret.setToExternal(ptr, strlen(ptr));
    }
    return ret;
}

static int blockingWrite(int fd, const void *buffer, size_t len) {
    size_t writeOffset = 0;
    const uint8_t *toWrite;
    toWrite = (const uint8_t *)buffer;
    while (writeOffset < len) {
        ssize_t written;
        do {
            written = write(fd, toWrite + writeOffset,
                                len - writeOffset);
            EMBMS_LOGD("send to:%d %d byte :%s", fd, (int)(len - writeOffset), toWrite + writeOffset);
        } while (written < 0 && ((errno == EINTR) || (errno == EAGAIN)));
        if (written >= 0) {
            writeOffset += written;
        } else {   // written < 0
            EMBMS_LOGE("Response: unexpected error on write errno:%d, %s",
                   errno, strerror(errno));
            close(fd);
            return -1;
        }
    }
    return 0;
}

sp<ILiteRadio> getLiteRadioProxy(int serviceId) {
    EMBMS_LOGD("getLiteRadioProxy");
    if (s_liteRadioProxy[serviceId] == NULL) {
        s_liteRadioProxy[serviceId] = ILiteRadio::getService(
                hidl_string(serviceId == 0 ? RADIO1_SERVICE_NAME : RADIO2_SERVICE_NAME));
    }
    EMBMS_LOGD("getLiteRadioProxy finish");
    return s_liteRadioProxy[serviceId];
}

struct LiteRadioResponseImpl : public ILiteRadioResponse {
    Return<void> sendCmdResponse(const LiteRadioResponseInfo& info,
            const hidl_string& response);
};

Return<void> LiteRadioResponseImpl::sendCmdResponse(const LiteRadioResponseInfo& info,
        const hidl_string& response) {
    int ret = -1;
    char *atResp = NULL;
    int serviceId = EMBMS;
    if (!copyHidlStringToCharPtr(&atResp, response.c_str())) {
        return Void();
    }
    EMBMS_LOGD("sendLiteCmdResponse atResp:%s", atResp);
    pthread_mutex_lock(&s_mainwriteMutex[serviceId]);
    ret = blockingWrite(s_fdClient[serviceId], atResp, strlen(atResp)+1);
    if (ret < 0) {
        EMBMS_LOGE("blockingWrite error");
    }
    pthread_mutex_unlock(&s_mainwriteMutex[serviceId]);
    free(atResp);
    return Void();
}

struct LiteRadioIndicationImpl : public ILiteRadioIndication {
    Return<void> sendCmdInd(LiteRadioIndicationType type, const hidl_string& data);
};

Return<void> LiteRadioIndicationImpl::sendCmdInd(LiteRadioIndicationType type,
        const hidl_string& data) {
    int ret = -1;
    int serviceId = EMBMS;
    char *atData = NULL;
    if (!copyHidlStringToCharPtr(&atData, data.c_str())) {
        return Void();
    }
    EMBMS_LOGD("sendCmdInd atData:%s", atData);
    pthread_mutex_lock(&s_mainwriteMutex[serviceId]);
    ret = blockingWrite(s_fdClient[serviceId], atData, strlen(atData)+1);
    if (ret < 0) {
        EMBMS_LOGE("blockingWrite error");
    }
    pthread_mutex_unlock(&s_mainwriteMutex[serviceId]);
    free(atData);
    return Void();
}

int getPhoneCount() {
    int simCount = 1;
    char prop[PROPERTY_VALUE_MAX] = {0};
    property_get("persist.radio.multisim.config", prop, "unknown");

    if (strcmp(prop, "dsds") == 0 || strcmp(prop, "dsda") == 0) {
        simCount = 2;
    } else if (strcmp(prop, "tsts") == 0) {
        simCount = 3;
    } else {
        simCount = 1;
    }
    return simCount;
}

static char *findNextEOL(char *cur) {
    if (cur[0] == '>' && cur[1] == ' ' && cur[2] == '\0') {
        return cur+2;  /* SMS prompt charFlver...not \r terminated */
    }
    while (*cur != '\0' && *cur != '\r' && *cur != '\n') cur++;

    return *cur == '\0' ? NULL : cur;
}

static const char *readline(ChannelInfo *chInfo) {
    ssize_t count, err_count = 0;
    char *p_read = NULL, *p_eol = NULL;

    if (*chInfo->s_respBufferCur == '\0') {  // empty buffer
        chInfo->s_respBufferCur = chInfo->s_respBuffer;
        *chInfo->s_respBufferCur = '\0';
        p_read = chInfo->s_respBuffer;
    } else {  // there's data in the buffer from the last read
        while (*chInfo->s_respBufferCur == '\r' || *chInfo->s_respBufferCur == '\n') {
            chInfo->s_respBufferCur++;
        }

        p_eol = findNextEOL(chInfo->s_respBufferCur);
        if (p_eol == NULL) {
            size_t len = strlen(chInfo->s_respBufferCur);
            memmove(chInfo->s_respBuffer, chInfo->s_respBufferCur, len + 1);
            p_read = chInfo->s_respBuffer + len;
            chInfo->s_respBufferCur = chInfo->s_respBuffer;
        }
    }

    while (p_eol == NULL) {
        if (0 == MAX_BUFFER_BYTES - (p_read - chInfo->s_respBuffer)) {
            chInfo->s_respBufferCur = chInfo->s_respBuffer;
            *chInfo->s_respBufferCur = '\0';
            p_read = chInfo->s_respBuffer;
        }

        do {
            count = read(chInfo->s_fd, p_read, MAX_BUFFER_BYTES - (p_read - chInfo->s_respBuffer));
        } while (count < 0 && errno == EINTR);

        if (count > 0) {
            p_read[count] = '\0';
            while (*chInfo->s_respBufferCur == '\r' || *chInfo->s_respBufferCur == '\n') {
                chInfo->s_respBufferCur++;
            }
            p_eol = findNextEOL(chInfo->s_respBufferCur);
            p_read += count;
            err_count = 0;
        } else if (count <= 0) {  /* read error encountered or EOF reached */
            if (count == 0) {
                err_count++;
                if (err_count > 10) {
                    sleep(10);
                } else {
                    EMBMS_LOGE("atchannel: EOF reached. err_count = %zd", err_count);
                }
            }
            return NULL;
        }
    }

    chInfo->line = chInfo->s_respBufferCur;
    *p_eol = '\0';
    chInfo->s_respBufferCur = p_eol + 1;
    if (strstr(chInfo->name, "embmsd") != NULL) {
        EMBMS_LOGD("embmsd read num = %d, buf = %s", (int)strlen(chInfo->line), chInfo->line);
    } else if (strstr(chInfo->name, "mdt_socket") != NULL) {
        EMBMS_LOGD("mdt_socket read num = %d, buf = %s", (int)strlen(chInfo->line), chInfo->line);
    } else {
        EMBMS_LOGD("%s: Recv < %s\n", chInfo->name, chInfo->line);
    }

    return chInfo->line;
}

static void *mainLoop(void *param) {
    int ret;
    int serviceId = -1;
    if (param) {
        serviceId= *((int*)param);
    }
    if (serviceId < 0 || serviceId >= MAX_SERVICE_NUM) {
        EMBMS_LOGE("serviceId=%d should be positive and less than %d, return",
               serviceId, MAX_SERVICE_NUM);
        return NULL;
    }
    s_fdListen[serviceId] = -1;

again:
    if (serviceId == EMBMS) {
        s_fdListen[serviceId] = android_get_control_socket(s_socketName[serviceId]);
    } else if (serviceId == MDT) {
        s_fdListen[serviceId] = socket_local_server(s_socketName[serviceId],
                ANDROID_SOCKET_NAMESPACE_ABSTRACT, SOCK_STREAM);
    }
    if (s_fdListen[serviceId] < 0) {
        EMBMS_LOGE("Failed to get socket %s,errno:%s", s_socketName[serviceId],strerror(errno));
        goto again;
    }

    ret = listen(s_fdListen[serviceId], 4);
    if (ret < 0) {
        EMBMS_LOGE("Failed to listen on control socket '%d': %s",
                s_fdListen[serviceId], strerror(errno));
        return NULL;
    }

    s_liteRadioResponse = new LiteRadioResponseImpl;
    s_liteRadioIndication = new LiteRadioIndicationImpl;
    s_liteRadioProxy[serviceId] = getLiteRadioProxy(serviceId);
    if (s_liteRadioProxy[serviceId] == NULL) {
        EMBMS_LOGE("embms failed to get connection to radio service, errno: %s", strerror(errno));
        return NULL;
    }
    Return<void> setStatus = s_liteRadioProxy[serviceId]->setResponseFunctions(s_liteRadioResponse, s_liteRadioIndication);
    if (!setStatus.isOk()) {
        EMBMS_LOGE("setResponseFunctions : rild service died");
        return NULL;
    }

    for (;;) {
        EMBMS_LOGD("[%s]mainLoop waiting for client..", s_serviceName[serviceId]);
        if ((s_fdClient[serviceId] = accept(s_fdListen[serviceId], NULL, NULL)) == -1) {
            sleep(1);
            continue;
        }

#if 1
        /* check the credential of the other side and only accept socket from
         * specified process
         */
        struct ucred creds;
        socklen_t szCreds = sizeof(creds);
        struct passwd *pwd = NULL;

        errno = 0;
        int is_phone_socket = 0;
        int err;

        EMBMS_LOGD("[%s]mainLoop processName:%s", s_serviceName[serviceId], s_processName[serviceId]);
        err = getsockopt(s_fdClient[serviceId], SOL_SOCKET, SO_PEERCRED, &creds, &szCreds);

        if (err == 0 && szCreds > 0) {
            errno = 0;
            pwd = getpwuid(creds.uid);
            if (pwd != NULL) {
                EMBMS_LOGD("[%s]pwd->pw_name: [%s]", s_serviceName[serviceId], pwd->pw_name);
            } else {
                EMBMS_LOGE("Error on getpwuid() errno: %d", errno);
            }
        } else {
            EMBMS_LOGD("Error on getsockopt() errno: %d", errno);
        }
#endif

        EMBMS_LOGD("[%s]mainLoop accept client:%d", s_serviceName[serviceId], s_fdClient[serviceId]);
        s_socketInfo[serviceId].s_fd = s_fdClient[serviceId];
        snprintf(s_socketInfo[serviceId].name, MAX_NAME_LENGTH, "%s", s_socketName[serviceId]);
        memset(s_socketInfo[serviceId].s_respBuffer, 0, MAX_BUFFER_BYTES + 1);
        s_socketInfo[serviceId].s_respBufferCur = s_socketInfo[serviceId].s_respBuffer;
        ChannelInfo *soInfo = &s_socketInfo[serviceId];

        while (1) {
            if (!readline(soInfo)) {
                break;
            }
            if (soInfo->line == NULL) {
                continue;
            }
            EMBMS_LOGD("receive:%s",soInfo->line);
            s_liteRadioProxy[serviceId]->sendCmd(serviceId, soInfo->line);
        }
        close(s_fdClient[serviceId]);
        s_fdClient[serviceId] = -1;
    }

    return NULL;
}

int main() {
    EMBMS_LOGD("start embms");
    pthread_t tid;
    pthread_attr_t attr;
    struct sigaction Flvion;
    int ret = -1;
    int serviceId = EMBMS;

    memset(&Flvion, 0x00, sizeof(Flvion));
    Flvion.sa_handler = SIG_IGN;
    Flvion.sa_flags = 0;
    sigemptyset(&Flvion.sa_mask);
    ret = sigaction(SIGPIPE, &Flvion, NULL);
    if (ret < 0) {
        EMBMS_LOGE("sigaction() failed!");
        exit(-1);
    }
    EMBMS_LOGD("start switchUser!!!");
    switchUser();

    sleep(7);
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&tid, &attr, mainLoop, (void*)&s_funcId[serviceId])
            < 0) {
        EMBMS_LOGE("Failed to create modemd listen accept thread");
    }
    do {
        pause();
    } while (1);

    return 0;
}
