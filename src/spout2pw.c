#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "spout2pw_unix.h"

#include <winbase.h>
#include <windef.h>
#include <winnt.h>
#include <winsvc.h>
#include <winuser.h>

#include <winioctl.h>

#include "wine/debug.h"
#include "wine/server.h"

#include <spoutdxtoc.h>

/* close(2): wine PE doesn't pull in <unistd.h>; forward declare just to
 * release the dmabuf fd we got from wine_server_handle_to_fd on error paths. */
extern int close(int fd);

WINE_DEFAULT_DEBUG_CHANNEL(spout2pw);

static WCHAR spout2pwW[] = L"Spout2Pw";
static HANDLE exit_event;
static SERVICE_STATUS_HANDLE service_handle;
static SERVICE_STATUS service_status;

static HANDLE sendernames_thread_handle = 0;
static SPOUTDXTOC_SENDERNAMES *spout_names = NULL;

static DWORD WINAPI sendernames_thread(void *arg);

static bool do_restart = false;

/*
 * Wine 11 removed the \??\SharedGpuResource device that Proton 10 and earlier
 * exposed for cross-process shared D3D11 texture import. The replacement is
 * the standard D3DKMT API, which routes through the wineserver via the
 * d3dkmt_object_open request. The wineserver-side object holds the dmabuf fd
 * for the underlying GPU allocation; wine_server_handle_to_fd extracts it.
 *
 * D3DKMT_RESOURCE = 6 matches enum d3dkmt_type in wine 11's
 * dlls/win32u/d3dkmt.c. The d3dkmt_dxgi_desc layout below mirrors the same
 * file's runtime descriptor struct.
 */
typedef UINT32 D3DKMT_HANDLE;
#define D3DKMT_RESOURCE 6

struct d3dkmt_dxgi_desc_v11 {
    UINT size;
    UINT version;
    UINT width;
    UINT height;
    UINT format;            /* DXGI_FORMAT */
    UINT unknown_0;
    UINT unknown_1;
    UINT keyed_mutex;
    D3DKMT_HANDLE mutex_handle;
    D3DKMT_HANDLE sync_handle;
    UINT nt_shared;
    UINT unknown_2;
    UINT unknown_3;
    UINT unknown_4;
};

struct receiver {
    char *name;
    void *source;
    SPOUTDXTOC_RECEIVER *spout;
    HANDLE thread;
    struct source_info info;
    bool force_update;
};

struct receiver **receivers;
size_t num_receivers = 0;

typedef enum D3D11_TEXTURE_LAYOUT {
    D3D11_TEXTURE_LAYOUT_UNDEFINED = 0,
    D3D11_TEXTURE_LAYOUT_ROW_MAJOR = 1,
    D3D11_TEXTURE_LAYOUT_64K_STANDARD_SWIZZLE = 2
} D3D11_TEXTURE_LAYOUT;

struct DxvkSharedTextureMetadata {
    UINT Width;
    UINT Height;
    UINT MipLevels;
    UINT ArraySize;
    DXGI_FORMAT Format;
    DXGI_SAMPLE_DESC SampleDesc;
    D3D11_USAGE Usage;
    UINT BindFlags;
    UINT CPUAccessFlags;
    UINT MiscFlags;
    D3D11_TEXTURE_LAYOUT TextureLayout;
};

static inline void init_unicode_string(UNICODE_STRING *str, const WCHAR *data) {
    str->Length = wcslen(data) * sizeof(WCHAR);
    str->MaximumLength = str->Length + sizeof(WCHAR);
    str->Buffer = (WCHAR *)data;
}

void show_error(HRESULT res, const char *msg) {
    if (!msg) {
        switch (res) {
        case STATUS_FATAL_APP_EXIT:
            msg = "Unknown fatal error";
            break;
        case STATUS_ACCESS_VIOLATION:
            msg = "Spout2PW crashed (access violation)";
            break;
        case STATUS_NO_SUCH_DEVICE:
            msg = "Device crashed or unavailable";
            break;
        case STATUS_NOT_SUPPORTED:
            msg = "Missing a required feature";
            break;
        default:
            msg = "Unknown error";
            break;
        }
    }

    char *dialog_msg = malloc(strlen(msg) + 256);
    sprintf(dialog_msg,
            "%s (%08lx)\n\n"
            "Please see https://lina.yt/s2pw-error for troubleshooting steps.",
            msg, (long)res);

    ERR("Error: %s\n", dialog_msg);

    // Kick the service status so the window is not closed automatically
    // too quickly.
    service_status.dwCheckPoint++;
    service_status.dwWaitHint = 30000;
    SetServiceStatus(service_handle, &service_status);

    TRACE("Show error message box\n");

    // Hack: https://bugs.winehq.org/show_bug.cgi?id=59393
    AllocConsole();

    MessageBoxA(NULL, dialog_msg, "Spout2PW error",
                MB_OK | MB_ICONERROR | MB_SERVICE_NOTIFICATION | MB_TOPMOST);
    TRACE("Message box returned\n");

    free(dialog_msg);
}

struct imported_resource {
    int fd;            /* dmabuf fd of the underlying GPU memory */
    UINT width;
    UINT height;
    UINT format;       /* DXGI_FORMAT */
};

/*
 * Open a KMT-shared GPU resource by issuing a d3dkmt_object_open wineserver
 * request directly, then extracting the dmabuf fd from the returned handle.
 * This replaces the Proton-10-only \??\SharedGpuResource IOCTL flow used in
 * older spout2pw builds.
 *
 * Returns 0 and fills *out on success; -1 on failure.
 */
static int import_shared_resource(D3DKMT_HANDLE kmt_handle,
                                  struct imported_resource *out) {
    HANDLE wine_handle = NULL;
    NTSTATUS status;
    int fd = -1;

    /*
     * Open the shared D3DKMT resource via the wineserver d3dkmt_object_open
     * request. The handler in server/d3dkmt.c requires
     *   runtime_size == 0  OR  runtime_size == object->runtime_size
     * for resources (anything else returns STATUS_INVALID_PARAMETER). We
     * don't know the runtime size in advance and don't actually need the
     * metadata (width/height/format are already populated from
     * SpoutDXToCGetSenderInfo above), so we deliberately *don't* call
     * wine_server_set_reply — that leaves runtime_size = 0, skipping the
     * size check, and we just grab the returned handle.
     *
     * Request-number override 306: CachyOS Proton 11 patches in 3 extra
     * wineserver requests (get_inproc_sync_fd, get_inproc_alert_fd,
     * fsync_free_shm_idx) before the d3dkmt block, shifting
     * REQ_d3dkmt_object_open from enum index 303 (stock wine 11.0) to 306.
     * Without this override we'd get STATUS_NOT_IMPLEMENTED. Derived
     * empirically from the Proton wineserver's req_handlers table.
     */
    SERVER_START_REQ(d3dkmt_object_open) {
        req->type = D3DKMT_RESOURCE;
        req->global = kmt_handle;
        req->handle = 0;
        __req.u.req.request_header.req = 306;
        status = wine_server_call(req);
        if (!status) wine_handle = wine_server_ptr_handle(reply->handle);
    }
    SERVER_END_REQ;

    if (status) {
        ERR("d3dkmt_object_open failed, status %#lx\n", (long int)status);
        return -1;
    }

    status = wine_server_handle_to_fd(wine_handle, GENERIC_ALL, &fd, NULL);
    NtClose(wine_handle);
    if (status != STATUS_SUCCESS) {
        ERR("wine_server_handle_to_fd failed, status %#lx\n", (long int)status);
        return -1;
    }

    out->fd = fd;
    out->width = 0;
    out->height = 0;
    out->format = 0;
    return 0;
}

static NTSTATUS WINAPI lock_texture(void *args, ULONG size) {
    struct receiver_params *params = args;
    struct receiver *receiver = params->receiver;
    SPOUTDXTOC_RECEIVER *recv = receiver->spout;
    struct lock_texture_return ret = {.retval = 0};

    if (!SpoutDXToCCheckTextureAccess(recv)) {
        ERR("Failed to lock shared texture\n");
        ret.retval = -1;
    } else {
        if (SpoutDXToCGetFrameCount(recv, &ret.frame_count)) {
            ret.flags |= FRAME_IS_NEW;
        }
    }

    return NtCallbackReturn(&ret, sizeof(ret), STATUS_SUCCESS);
}

static NTSTATUS WINAPI unlock_texture(void *args, ULONG size) {
    struct receiver_params *params = args;
    struct receiver *receiver = params->receiver;
    SPOUTDXTOC_RECEIVER *spout = receiver->spout;

    SpoutDXToCAllowTextureAccess(spout);

    return NtCallbackReturn(NULL, 0, STATUS_SUCCESS);
}

static void trigger_restart(void) {
    TRACE("Restarting service due to error\n");
    do_restart = true;
    SetEvent(exit_event);
}

static DWORD WINAPI receiver_thread(void *arg) {
    struct receiver *receiver = arg;

    TRACE("Receiver thread starting for %s\n", receiver->name);
    UNIX_CALL(run_source, receiver->source);
    TRACE("Receiver thread exiting for %s\n", receiver->name);

    return STATUS_SUCCESS;
}

static struct source_info get_receiver_info(struct receiver *receiver) {
    SPOUTDXTOC_RECEIVER *spout = receiver->spout;
    SPOUTDXTOC_SENDERINFO info;
    struct source_info ret = {.opaque_fd = -1};

    TRACE("Updating receiver %p -> %p (%s)\n", receiver, spout, receiver->name);

    if (!SpoutDXToCIsConnected(spout)) {
        ret.flags = RECEIVER_DISCONNECTED;
        TRACE("-> Not connected\n");
        return ret;
    }

    if (!SpoutDXToCGetSenderInfo(spout, &info)) {
        ret.flags = RECEIVER_DISCONNECTED;
        TRACE("-> Failed to get sender info (disconnected?)\n");
        return ret;
    }

    HANDLE share_handle = info.shareHandle;

    TRACE("Sender %s: %dx%d fmt=%d handle=0x%lx usage=0x%x changed=%d\n",
          receiver->name, info.width, info.height, info.format,
          (long)(intptr_t)info.shareHandle, info.usage, info.changed);

    ret.width = info.width;
    ret.height = info.height;
    ret.format = info.format;
    ret.usage = info.usage;

    if (!info.changed && !receiver->force_update)
        return ret;

    receiver->force_update = true;

    struct imported_resource imp;
    if (import_shared_resource((D3DKMT_HANDLE)(uintptr_t)info.shareHandle, &imp) < 0) {
        ret.flags |= RECEIVER_TEXTURE_INVALID;
        WARN("Share handle import failed\n");
        return ret;
    }

    TRACE("Share handle imported: 0x%lx -> fd %d (%ux%u format=%u)\n",
          HandleToLong(share_handle), imp.fd, imp.width, imp.height, imp.format);

    Sleep(50);

    if (!SpoutDXToCGetSenderInfo(spout, &info) ||
        info.shareHandle != share_handle) {
        WARN("Texture changed out under us, trying again later (0x%lx -> "
             "0x%lx)\n",
             HandleToLong(share_handle), HandleToLong(info.shareHandle));
        ret.flags |= RECEIVER_TEXTURE_INVALID;
        close(imp.fd);
        return ret;
    }

    ret.width = info.width;
    ret.height = info.height;
    ret.format = info.format;
    ret.usage = info.usage;

    TRACE("Update DX Texture\n");
    if (!SpoutDXToCUpdateDXTexture(spout, &info)) {
        WARN("Failed to update DX texture\n");
        ret.flags |= RECEIVER_TEXTURE_INVALID;
        close(imp.fd);
        return ret;
    }

    /*
     * Wine 11's d3dkmt_object_open reply returns the texture's width/height/
     * format inline in the runtime descriptor — no separate metadata IOCTL.
     * For D3D11 resources the underlying GPU allocation size isn't exposed
     * (only D3D12 desc carries resource_size); leave it 0 and let the UNIX
     * side compute the right Vulkan allocation from width × height × format.
     */
    if (imp.width && imp.height) {
        ret.width = imp.width;
        ret.height = imp.height;
        ret.format = imp.format;
    }
    ret.resource_size = 0;
    ret.bind_flags = 0;

    TRACE("New texture OPAQUE fd: %d\n", imp.fd);

    ret.opaque_fd = imp.fd;
    ret.flags |= RECEIVER_TEXTURE_UPDATED;
    receiver->force_update = false;

    return ret;
}

static void update_receiver(struct receiver *receiver) {
    struct source_info new_info = get_receiver_info(receiver);

    if (!receiver->source) {
        if (new_info.flags == RECEIVER_TEXTURE_UPDATED) {
            struct create_source_params params = {
                .sender_name = receiver->name,
                .receiver = receiver,
                .info = new_info,
            };
            TRACE("Creating source\n");
            NTSTATUS ret = UNIX_CALL(create_source, &params);
            receiver->source = params.ret_source;
            if (receiver->source) {
                receiver->thread =
                    CreateThread(NULL, 0, receiver_thread, receiver, 0, 0);
            } else {
                TRACE("Source creation failed: 0x%lx %s\n", ret,
                      params.error_msg);
                show_error(ret, params.error_msg);
            }
        }
        return;
    }

    if (new_info.flags != receiver->info.flags ||
        (new_info.flags & RECEIVER_TEXTURE_UPDATED)) {
        struct update_source_params params = {
            .source = receiver->source,
            .info = new_info,
        };
        NTSTATUS ret = UNIX_CALL(update_source, &params);
        if (ret == STATUS_NO_SUCH_DEVICE) {
            ERR("Source '%s' had a fatal error\n", receiver->name);
            trigger_restart();
            return;
        }
        receiver->info = new_info;
    }
}

static void update_receivers(void) {
    for (uint32_t i = 0; i < num_receivers; i++)
        update_receiver(receivers[i]);
}

static struct receiver *find_receiver(const char *name) {
    for (uint32_t i = 0; i < num_receivers; i++)
        if (!strcmp(receivers[i]->name, name))
            return receivers[i];
    return NULL;
}

static void add_receiver(const char *name) {
    SPOUTDXTOC_RECEIVER *spout = SpoutDXToCNewReceiver(name);
    if (!spout) {
        TRACE("Failed to create receiver for %s\n", name);
        return;
    }

    struct receiver *receiver = calloc(1, sizeof(struct receiver));

    receiver->name = strdup(name);
    receiver->source = NULL;
    receiver->spout = spout;
    receiver->thread = NULL;

    num_receivers++;
    receivers = realloc(receivers, sizeof(struct receiver) * num_receivers);
    receivers[num_receivers - 1] = receiver;
}

static void remove_receiver(struct receiver *receiver) {
    TRACE("Destroying source %s\n", receiver->name);
    if (receiver->source)
        UNIX_CALL(destroy_source, receiver->source);

    TRACE("Joining thread for %s\n", receiver->name);
    if (receiver->thread)
        WaitForSingleObject(receiver->thread, INFINITE);

    TRACE("Freeing receiver for %s\n", receiver->name);
    SpoutDXToCFreeReceiver(receiver->spout);

    for (uint32_t i = 0; i < num_receivers; i++) {
        if (receivers[i] == receiver) {
            memmove(&receivers[i], &receivers[i + 1],
                    sizeof(struct receiver) * (num_receivers - i - 1));
            num_receivers--;
            goto free;
        }
    }
    ERR("Did not find receiver %p (%s)\n", receiver, receiver->name);

free:
    TRACE("Done removing %s\n", receiver->name);
    free(receiver->name);
    free(receiver);
}

static DWORD WINAPI sendernames_thread(void *arg) {
    TRACE("Sendernames thread started\n");

    SPOUTDXTOC_NAMELIST list = {0};
    do {
        SPOUTDXTOC_NAMELIST new_list = {0};
        SPOUTDXTOC_NAMELIST added = {0};
        SPOUTDXTOC_NAMELIST removed = {0};

        if (!SpoutDXToCGetSenderList(spout_names, &list, &new_list, &added,
                                     &removed)) {
            SpoutDXToCNamelistClear(&new_list);
            update_receivers();
            continue;
        }

        TRACE("Sender list changed\n");

        for (uint32_t i = 0; i < removed.count; i++) {
            TRACE("Removed sender: %s\n", removed.list[i]);
            struct receiver *receiver = find_receiver(removed.list[i]);
            if (receiver)
                remove_receiver(receiver);
        }

        for (uint32_t i = 0; i < added.count; i++) {
            TRACE("New sender: %s\n", added.list[i]);
            add_receiver(added.list[i]);
        }

        SpoutDXToCNamelistClear(&list);
        SpoutDXToCNamelistClear(&added);
        SpoutDXToCNamelistClear(&removed);
        list = new_list;

        update_receivers();
    } while (WaitForSingleObject(exit_event, 100) == WAIT_TIMEOUT);

    TRACE("Sendernames thread returning\n");

    while (num_receivers)
        remove_receiver(receivers[num_receivers - 1]);

    TRACE("Sendernames thread exit\n");

    return STATUS_SUCCESS;
}

static DWORD WINAPI service_handler(DWORD ctrl, DWORD event_type,
                                    LPVOID event_data, LPVOID context) {
    switch (ctrl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        TRACE("Service control: Shutting down\n");
        service_status.dwCurrentState = SERVICE_STOP_PENDING;
        service_status.dwControlsAccepted = 0;
        SetServiceStatus(service_handle, &service_status);
        SetEvent(exit_event);
        return NO_ERROR;

    default:
        FIXME("Got service ctrl %lx\n", (long)ctrl);
        SetServiceStatus(service_handle, &service_status);
        return NO_ERROR;
    }
}

// Future use
__attribute__((unused)) static const char *_getenv(const char *var) {
    NTSTATUS ret;
    struct getenv_params params = {.var = var};

    ret = UNIX_CALL(getenv, &params);
    if (ret != STATUS_SUCCESS) {
        TRACE("unix_getenv(%s) failed (0x%lx)\n", var, ret);
        return NULL;
    }

    return params.val;
}

static void WINAPI ServiceMain(DWORD argc, LPWSTR *argv) {
    NTSTATUS ret;
    const char *msg = NULL;

    service_handle =
        RegisterServiceCtrlHandlerExW(spout2pwW, service_handler, NULL);
    if (!service_handle)
        return;

    service_status.dwServiceType = SERVICE_WIN32;
    service_status.dwCurrentState = SERVICE_START_PENDING;
    service_status.dwControlsAccepted = 0;
    service_status.dwWin32ExitCode = 0;
    service_status.dwServiceSpecificExitCode = 0;
    service_status.dwCheckPoint = 1;
    service_status.dwWaitHint = 15000;
    SetServiceStatus(service_handle, &service_status);

    TRACE("Loading unix calls\n");

    ret = __wine_init_unix_call();
    if (ret != STATUS_SUCCESS) {
        msg = "Error initializing UNIX library";
        goto stop;
    }

    TRACE("Initializing spoutdxtoc.dll\n");

restart:

    // NOTE: There is no point continuing if it is.
    spout_names = SpoutDXToCNewSenderNames();
    if (spout_names == NULL) {
        msg = "Error initializing spoutdxtoc.dll";
        goto stop;
    }

    TRACE("Starting up libfunnel\n");

    struct startup_params params = {
        .lock_texture = (UINT_PTR)lock_texture,
        .unlock_texture = (UINT_PTR)unlock_texture,
        .error_msg = NULL,
    };

    ret = UNIX_CALL(startup, &params);
    if (ret != STATUS_SUCCESS) {
        msg = params.error_msg;
        goto stop;
    }

    TRACE("Starting service\n");

    exit_event = CreateEventW(NULL, TRUE, FALSE, NULL);

    TRACE("Starting sendernames thread\n");
    sendernames_thread_handle =
        CreateThread(NULL, 0, sendernames_thread, NULL, 0, 0);
    TRACE("Sendernames thread created\n");

    service_status.dwCurrentState = SERVICE_RUNNING;
    service_status.dwControlsAccepted =
        SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    service_status.dwCheckPoint = 0;
    service_status.dwWaitHint = 0;
    SetServiceStatus(service_handle, &service_status);

    TRACE("Waiting for exit event\n");
    WaitForMultipleObjects(1, &exit_event, FALSE, INFINITE);

    SetEvent(exit_event);

    if (sendernames_thread_handle != NULL) {
        TRACE("Stopping sender names thread\n");
        WaitForSingleObject(sendernames_thread_handle, INFINITE);
    }

    TRACE("Shutting down libfunnel\n");
    UNIX_CALL(teardown, NULL);

    TRACE("Freeing sender names\n");
    SpoutDXToCFreeSenderNames(spout_names);

    if (do_restart) {
        do_restart = false;
        goto restart;
    }

stop:
    if (ret != STATUS_SUCCESS) {
        show_error(ret, msg);
    }

    FreeConsole();
    service_status.dwCurrentState = SERVICE_STOPPED;
    service_status.dwControlsAccepted = 0;
    service_status.dwCheckPoint = 0;
    service_status.dwWaitHint = 0;
    SetServiceStatus(service_handle, &service_status);

    TRACE("Service stopped\n");
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow) {
    static const SERVICE_TABLE_ENTRYW service_table[] = {
        {spout2pwW, ServiceMain}, {NULL, NULL}};

    bool found = false;
    for (int i = 0; i < 100; i++) {
        char buf[16];
        sprintf(buf, "WINEDLLDIR%d", i);
        const char *val = getenv(buf);
        if (!val)
            break;
        TRACE("Check DLL path: %s=%s\n", buf, val);
        size_t len = strlen(val);
        const char *match = "\\spout2pw-dlls";
        size_t mlen = strlen(match);
        if (len < mlen)
            continue;
        if (strcmp(val + len - mlen, match))
            continue;
        TRACE("Spout2PW DLL path found\n");
        found = 1;
        break;
    }

    if (!found) {
        ERR("Spout2 not configured in WINEDLLPATH\n");
        return 0;
    }

    TRACE("Starting service ctrl\n");

    StartServiceCtrlDispatcherW(service_table);

    TRACE("WinMain returning\n");
    return 0;
}
