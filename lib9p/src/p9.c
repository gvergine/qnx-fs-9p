/* lib9p — version, names and error strings. */
#include "lib9p/p9.h"

#include <string.h>

const char *p9_proto_version(void)
{
    return P9_PROTO_VERSION;
}

p9_str_t p9_str(const char *s)
{
    p9_str_t r = {s, strlen(s)};
    return r;
}

const char *p9_strerror(int err)
{
    switch (err) {
    case P9_OK:
        return "ok";
    case P9_E_NOSPACE:
        return "buffer too small";
    case P9_E_SHORT:
        return "message truncated";
    case P9_E_PROTO:
        return "malformed message";
    case P9_E_TYPE:
        return "unexpected message type";
    case P9_E_INVAL:
        return "invalid argument";
    case P9_E_EXHAUSTED:
        return "no free id";
    default:
        return "unknown error";
    }
}

const char *p9_msg_name(uint8_t type)
{
    switch (type) {
    case P9_TLERROR:
        return "Tlerror";
    case P9_RLERROR:
        return "Rlerror";
    case P9_TSTATFS:
        return "Tstatfs";
    case P9_RSTATFS:
        return "Rstatfs";
    case P9_TLOPEN:
        return "Tlopen";
    case P9_RLOPEN:
        return "Rlopen";
    case P9_TREADLINK:
        return "Treadlink";
    case P9_RREADLINK:
        return "Rreadlink";
    case P9_TGETATTR:
        return "Tgetattr";
    case P9_RGETATTR:
        return "Rgetattr";
    case P9_TREADDIR:
        return "Treaddir";
    case P9_RREADDIR:
        return "Rreaddir";
    case P9_TVERSION:
        return "Tversion";
    case P9_RVERSION:
        return "Rversion";
    case P9_TATTACH:
        return "Tattach";
    case P9_RATTACH:
        return "Rattach";
    case P9_TWALK:
        return "Twalk";
    case P9_RWALK:
        return "Rwalk";
    case P9_TREAD:
        return "Tread";
    case P9_RREAD:
        return "Rread";
    case P9_TCLUNK:
        return "Tclunk";
    case P9_RCLUNK:
        return "Rclunk";
    case P9_TLCREATE:
        return "Tlcreate";
    case P9_RLCREATE:
        return "Rlcreate";
    case P9_TSYMLINK:
        return "Tsymlink";
    case P9_RSYMLINK:
        return "Rsymlink";
    case P9_TSETATTR:
        return "Tsetattr";
    case P9_RSETATTR:
        return "Rsetattr";
    case P9_TFSYNC:
        return "Tfsync";
    case P9_RFSYNC:
        return "Rfsync";
    case P9_TLINK:
        return "Tlink";
    case P9_RLINK:
        return "Rlink";
    case P9_TMKDIR:
        return "Tmkdir";
    case P9_RMKDIR:
        return "Rmkdir";
    case P9_TRENAMEAT:
        return "Trenameat";
    case P9_RRENAMEAT:
        return "Rrenameat";
    case P9_TUNLINKAT:
        return "Tunlinkat";
    case P9_RUNLINKAT:
        return "Runlinkat";
    case P9_TWRITE:
        return "Twrite";
    case P9_RWRITE:
        return "Rwrite";
    default:
        return NULL;
    }
}
