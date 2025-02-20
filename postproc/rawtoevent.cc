// Little program to turn raw binary dclab trace files into Ascii event listings
// The main work is turning truncated cycle times into multiples of 10ns
// Copyright 2021 Richard L. Sites
//
// Input has filename like 
//   kutrace_control_20170821_095154_dclab-1_2056.trace
//
// compile with g++ -O2 rawtoevent.cc from_base40.cc kutrace_lib.cc -o rawtoevent
//
// To see raw trace in hex, use
//   od -Ax -tx8z -w32 foo.trace
//
// dsites 2022.05.23 Add FreeBSD tweaks
// dsites 2022.08.19 Add RISC-V tweaks
// dsites 2022.08.19 Add RPi tweaks
// dsites 2023.04.30 Update TSDELTA processing to go backward
// dsites 2023.05.03 Update timestamp processing to go backward in top 7/8 of wrap period
//


#include <map>
#include <set>
#include <string>

#include <stdio.h>
#include <stdlib.h>     // exit
#include <string.h>
#include <time.h>
#include <unistd.h>     // getpid gethostname
#include <sys/time.h>   // gettimeofday
#include <sys/types.h>

#include "basetypes.h"
#include "from_base40.h"
////#include "kutrace_control_names.h"
#include "kutrace_lib.h"

// To come from names in trace
static int gTIMER_IRQ_EVENT = 0x05ec;	// local_timer
static int gSCHED_EVENT = 0x0dff;	// -sched-

// Remap high syscall numbers into lo sys32 numbers
#define	kutrace_map_nr(nr) (nr + (nr & 0x200)) 

/* Amount to shift cycle counter to get 20-bit timestamps */
/* 4 bits = ~ 2.56 GHz/16 ~ 6nsec tick resolution */
/* 6 bits = ~ 2.56 GHz/64 ~ 24nsec tick resolution */
/* 8 bits = ~ 2.56 GHz/256 ~ 100nsec tick resolution */
/* 12 bits = ~ 2.56 GHz/4096 ~ 1.6 usec tick resolution */
/* THIS MUST MATCH the value in the kernel tracing module/code */

// Global for debugging
bool verbose = false;
bool hexevent = false;

//VERYTEMP
bool keep_idle = false;

//VERYTEMP
//static const uint64 FINDME = 1305990942;
static const uint64 FINDME = 0;

static const bool TRACEWRAP = false;
static const int kMAX_CPUS = 80;
static const int mhz_32bit_counts = 54;
static const int kNetworkMbPerSec = 1000;	// Default: 1 Gb/s
static const int kDefaultLowResNsec10 = 35;	// Low-res riscv: 0 dur => 350 nsec instead 

// For sanity checks
static const uint64 usec_per_100_years = 1000000LL * 86400 * 365 * 100;  // Thru ~2070

// Large ts difference means slightly backward time
static const uint64 kLargeTsdelta = 2000000000;	

// For dealing with riscv poor-resolution sifive u74-mc clock (1MHz)
bool is_low_res_ts = false;		// True for Riscv u74 1 MHz timestamp


// For deciding that large timestamp advance is really a late store with backward time.
static const uint64 kLateStoreThresh = 0x0000000000020000LLU;

// Version 3 all values are pre-shifted

#define IPC_Flag     0x80
#define WRAP_Flag    0x40
#define Unused2_Flag 0x20
#define Unused1_Flag 0x10
#define VERSION_MASK 0x0F

#define RDTSC_SHIFT 0 
#define OLD_RDTSC_SHIFT 6


// Module, control must be at least version 3 
static const int kRawVersionNumber = 3;

static const char* kIdleName = "-idle-";


// Very first block layout June 2018, called 12/6 headers
// Enables wraparound
// flags = x3 hex
//   +-------+-----------------------+-------------------------------+
//   | cpu#  |                  cycle counter                        | 0 module
//   +-------+-----------------------+-------------------------------+
//   | flags |                  gettimeofday                         | 1 DoDump
//   +-------+-----------------------+-------------------------------+
//   |                      start cycle counter                      | 2 DoDump
//   +-------------------------------+-------------------------------+
//   |                      start gettimeofday                       | 3 DoDump
//   +-------------------------------+-------------------------------+
//   |                       stop cycle counter                      | 4 DoDump
//   +-------------------------------+-------------------------------+
//   |                       stop gettimeofday                       | 5 DoDump
//   +-------------------------------+-------------------------------+
//   |                          u n u s e d                          | 6
//   +-------------------------------+-------------------------------+
//   |                          u n u s e d                          | 7
//   +===============================+===============================+
//   |           u n u s e d         |            PID                | 8  module
//   +-------------------------------+-------------------------------+
//   |                          u n u s e d                          | 9  module
//   +-------------------------------+-------------------------------+
//   |                                                               | 10 module
//   +                            pidname                            +
//   |                                                               | 11 module
//   +-------------------------------+-------------------------------+
//   |    followed by trace entries...                               |
//   ~                                                               ~
//
//
// All other blocks layout June 2018
//   +-------+-----------------------+-------------------------------+
//   | cpu#  |                  cycle counter                        | 0 module
//   +-------+-----------------------+-------------------------------+
//   | flags |                  gettimeofday                         | 1 DoDump
//   +===============================+===============================+
//   |           u n u s e d         |            PID                | 2 module
//   +-------------------------------+-------------------------------+
//   |                          u n u s e d                          | 3 module
//   +-------------------------------+-------------------------------+
//   |                                                               | 4 module
//   +                            pidname                            +
//   |                                                               | 5 module
//   +-------------------------------+-------------------------------+
//   |    followed by trace entries...                               |
//   ~                                                               ~
//


// MWAIT notes:
// $ cat /proc/cpuinfo
//   processor	: 0
//   vendor_id	: GenuineIntel
//   cpu family	: 6
//   model		: 60  ==> 0x3C
//   model name	: Intel(R) Celeron(R) CPU G1840 @ 2.80GHz
//
// ./drivers/idle/intel_idle.c
//   ICPU(0x3c, idle_cpu_hsw),

// static struct cpuidle_state hsw_cstates[] = {
// These latencies are documented as usec, but I think they are 100ns increments...
//  mwait(32), hda_29 13.9us  table: 133
//  mwait(32), hda_29 13.3us  table: 133
//  mwait(16), hda_29  4.0us  table: 33
//  mwait(16), hda_29  3.75us table: 33
//  mwait(1),  hda_29  1.74us table: 10
//  mwait(1),  hda_29  1.76us table: 10

//   "C1-HSW",  0x00, .exit_latency = 2,        // usec ?
//   "C1E-HSW", 0x01, .exit_latency = 10,
//   "C3-HSW",  0x10, .exit_latency = 33,
//   "C6-HSW",  0x20, .exit_latency = 133,
//   "C7s-HSW", 0x32, .exit_latency = 166,
//   "C8-HSW",  0x40, .exit_latency = 300,
//   "C9-HSW",  0x50, .exit_latency = 600,
//   "C10-HSW", 0x60, .exit_latency = 2600,



using std::map;
using std::set;
using std::string;

static double kDefaultSlope = 0.000285714;  // 1/3500, dclab-3 at 3.5 GHz

// Number of uint64 values per trace block
static const int kTraceBufSize = 8192;
// Number trace blocks per MB
static const double kTraceBlocksPerMB = 16.0;

static const char* soft_irq_name[] = {
  "hi", "timer", "tx", "rx",   "block", "irq_p", "taskl", "sched", 
  "hrtim", "rcu", "", "",      "", "", "", "ast"
};

const char* missingeventname[16] = {
  "nam#", "nam#", "spl#", "spl#",
  "trp#", "irq#", "/trp#", "/irq#",
  "sys#", "sys#", "/sys#", "/sys#",
  "s32#", "s32#", "/s32#", "/s32#",
};

typedef map<uint64, string> U64toString;
typedef set<uint64> U64set;

// These all use a single static buffer. In real production code, these would 
// all be std::string values, or something else at least as safe.
static const int kMaxDateTimeBuffer = 32;
static char gTempDateTimeBuffer[kMaxDateTimeBuffer];

static const int kMaxPrintBuffer = 256;
static char gTempPrintBuffer[kMaxPrintBuffer];

// F(cycles) gives usec = base_usec + (cycles - base_cycles) * m;
typedef struct {
  uint64 base_cycles;
  uint64 base_usec;
  uint64 base_cycles10;
  uint64 base_nsec10;
  double m_slope;
  double m_slope_nsec10;
} CyclesToUsecParams;

void SetParams(int64 start_cycles, int64 start_usec, 
               int64 stop_cycles, int64 stop_usec, CyclesToUsecParams* params) {
  params->base_cycles = start_cycles;
  params->base_usec = start_usec;
  if (stop_cycles <= start_cycles) {stop_cycles = start_cycles + 1;}	// avoid zdiv
  params->m_slope = (stop_usec - start_usec) * 1.0 / (stop_cycles - start_cycles);
  params->m_slope_nsec10 = params->m_slope * 100.0;
  if (verbose) {
    fprintf(stdout, "SetParams maps %18lldcy ==> %18lldus\n", start_cycles, start_usec);
    fprintf(stdout, "SetParams maps %18lldcy ==> %18lldus\n", stop_cycles, stop_usec);
    fprintf(stdout, "          diff %18lldcy ==> %18lldus\n", stop_cycles - start_cycles, stop_usec - start_usec);
    // Assume that cy increments every 64 CPU cycles
    fprintf(stdout, "SetParams slope %f us/cy (%f MHz)\n", params->m_slope, 64.0/params->m_slope);
  }
}

void SetParams10(int64 start_cycles10, int64 start_nsec10, CyclesToUsecParams* params) {
  params->base_cycles10 = start_cycles10;
  params->base_nsec10 = start_nsec10;
  if (verbose) {
    fprintf(stdout, "SetParams10 maps %16lldcy ==> %lldns10\n", start_cycles10, start_nsec10);
  }
}

int64 CyclesToUsec(int64 cycles, const CyclesToUsecParams& params) {
  int64 delta_usec = (cycles - params.base_cycles) * params.m_slope;
  return params.base_usec + delta_usec;
}

uint64 CyclesToNsec10(uint64 cycles, CyclesToUsecParams& params) {
  int64 delta_nsec10 = (cycles - params.base_cycles10) * params.m_slope_nsec10;
  return params.base_nsec10 + delta_nsec10;
}

int64 UsecToCycles(int64 usec, CyclesToUsecParams& params) {
  int64 delta_cycles = (usec - params.base_usec);
  delta_cycles /= params.m_slope;  // Combining above fails to convert double=>int64
  return params.base_cycles + delta_cycles;
}


// Turn seconds since the epoch into date_hh:mm:ss
// Not valid after January 19, 2038
const char* FormatSecondsDateTime(int32 sec) {
  if (sec == 0) {return "unknown";}  // Longer spelling: caller expecting date
  time_t tt = sec;
  struct tm* t = localtime(&tt);
  sprintf(gTempDateTimeBuffer, "%04d-%02d-%02d_%02d:%02d:%02d", 
         t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, 
         t->tm_hour, t->tm_min, t->tm_sec);
  return gTempDateTimeBuffer;
}

// Turn usec since the epoch into date_hh:mm:ss.usec
const char* FormatUsecDateTime(int64 us) {
  if (us == 0) {return "unknown";}  // Longer spelling: caller expecting date
  int32 seconds = us / 1000000;
  int32 usec = us - (seconds * 1000000);
  snprintf(gTempPrintBuffer, kMaxPrintBuffer, "%s.%06d", 
           FormatSecondsDateTime(seconds), usec);
  return gTempPrintBuffer;
}

// We wrapped if prior > now, except that we allow a modest amount of going backwards
// because an interrupt entry can get recorded in the midst of recording say a
// syscallentry, in which case the stored irq entry's timestamp may be later than
// the subsequently-written syscall entry's timestamp. We allow 4K counts backward
// (about 80 usec at nominal 20 ns/count). Count increment should be kept between
// 10 nsec and 40 nsec.

inline bool Wrapped(uint64 prior, uint64 now) {
  if (prior <= now) {return false;}	// Common case 
  return (prior > (now + 4096));	// Wrapped if prior is larger
}

inline bool LateStore(uint64 prior, uint64 now) {
  if (prior <= now) {return false;}		// Common case 
  return (prior <= (now + kLateStoreThresh));	// Late store 
}
 
// A user-mode-execution event is the pid number plus 64K
uint64 PidToEvent(uint64 pid) {return (pid & 0xFFFF) | 0x10000;}
uint64 EventToPid(uint64 event) {return event & 0xFFFF;}

// Event tests
inline bool is_cpu_description(uint64 event) {
  if (event == KUTRACE_MBIT_SEC) {return true;}
  return false;
}

// Return true if the event is user-mode execution 
inline bool is_contextswitch(uint64 event) {return (event == KUTRACE_USERPID);}

// Return true if the event is the idle task, pid 0 
inline bool is_idle(uint64 event) {return (event == 0x10000);}

// Return true if the event is user-mode execution 
inline bool is_usermode(uint64 event) {return (event > 0xffff) && !is_idle(event);}

// Return true if the event is a syscall/interrupt/trap 
inline bool is_call(uint64 event) {return (event <= 0xffff) && (KUTRACE_TRAP <= event) && ((event & 0x0200) == 0);}

// Return true if the event is an optimized syscall/interrupt/trap with included return 
inline bool is_opt_call(uint64 event, uint64 delta_t) {return (delta_t > 0) && is_call(event);}

// Return true if the event is a syscall/interrupt/trap return
inline bool is_return(uint64 event) {return (event <= 0xffff) && (KUTRACE_TRAP <= event) && ((event & 0x0200) != 0);}

// Return true if the event is a time pair
inline bool is_timepair(uint64 event) {return (event & ~0x0f0) == KUTRACE_TIMEPAIR;}

// Return true if the event is a name definition
inline bool is_namedef(uint64 event) {return (0x010 <= event) && (event <= 0x1ff) && (event != KUTRACE_PC_TEMP);}

// Return true if the name event is a PID name definition
inline bool is_pidnamedef(uint64 event) {return (event & 0xf0f) == 0x002;}

// Return true if the name event is a method name definition
inline bool is_methodnamedef(uint64 event) {return (event & 0xf0f) == 0x003;}

// Return true if the name event is a lock name definition
inline bool is_locknamedef(uint64 event) {return (event & 0xf0f) == 0x007;}

// Return true if the name event is the kernel version
inline bool is_kernelnamedef(uint64 event) {return (event & 0xf0f) == KUTRACE_KERNEL_VER;}

// Return true if the name event is the CPU model name
inline bool is_modelnamedef(uint64 event) {return (event & 0xf0f) == KUTRACE_MODEL_NAME;}

// Return true if the name event is the CPU model name
inline bool is_hostnamedef(uint64 event) {return (event & 0xf0f) == KUTRACE_HOST_NAME;}

// Return true if the name event is the CPU model name
inline bool is_queuenamedef(uint64 event) {return (event & 0xf0f) == KUTRACE_QUEUE_NAME;}

// Return true if the name event is the CPU model name
inline bool is_resnamedef(uint64 event) {return (event & 0xf0f) == KUTRACE_RES_NAME;}


// Return true if the event is a special marker (but not UserPidNum)
inline bool is_special(uint64 event) {return (0x0200 < event) && (event <= KUTRACE_MAX_SPECIAL);}

// Return true if the event is mark_a .. mark_d
inline bool is_mark(uint64 event) {return ((0x020A <= event) && (event <= 0x020D));}

// Return true if the event is mark_a mark_b mark_c
inline bool is_mark_abc(uint64 event) {
  return (event == 0x020A) || (event == 0x020B) || (event == 0x020C);
}

// Return true if the event is PC or PC_TEMP
inline bool is_pc_sample(uint64 event) {
  return (event == KUTRACE_PC_U) || (event == KUTRACE_PC_K) || (event == KUTRACE_PC_TEMP);
}

// Return true if the event is a local timer, for PC start_ts fixup
inline bool is_timer_irq(uint64 event) {
  return (event == gTIMER_IRQ_EVENT);
}

// Return true if the event is rpcreq, rpcresp, rpcmid, rpcrxpkt, rpxtxpkt,
inline bool has_rpcid(uint64 event) {
  return (KUTRACE_RPCIDREQ <= event) && (event <= KUTRACE_RPCIDTXMSG);
}

// Return true if the event is raw kernel packet receive/send time and hash
inline bool is_raw_pkt_hash(uint64 event) {
  return (KUTRACE_RX_PKT <= event) && (event <= KUTRACE_TX_PKT);
}

// Return true if the event is user message receive/send time and hash
inline bool is_user_msg_hash(uint64 event) {
  return (KUTRACE_RX_USER <= event) && (event <= KUTRACE_TX_USER);
}

// Return true if the event is RPC message processing begin/end 
inline bool is_rpc_msg(uint64 event) {
  return (KUTRACE_RPCIDREQ <= event) && (event <= KUTRACE_RPCIDRESP);
}

// Return true if the event is lock special
inline bool is_lock(uint64 event) {
  return (KUTRACE_LOCKNOACQUIRE <= event) && (event <= KUTRACE_LOCKWAKEUP);
}

// Return true if this event is irq call/ret to bottom half soft_irq handler (BH)
inline bool is_bottom_half(uint64 event) {
  return (event & ~0x0200) == (KUTRACE_IRQ + KUTRACE_BOTTOM_HALF);
}



int TracefileVersion(uint8 flags) {
  return flags & VERSION_MASK;
}

int HasIPC(uint8 flags) {
  return (flags & IPC_Flag) != 0;
}

int HasWraparound(uint8 flags) {
  return (flags & WRAP_Flag) != 0;
}


// Change any spaces and non-Ascii to underscore
// time dur event pid name(event)
void OutputName(FILE* f, uint64 nsec10, uint64 event, uint32 argall, const char* name) {
  // Avoid crazy big times
  if (nsec10 >= 99900000000LL) {
    if (verbose) {fprintf(stdout, "BUG ts=%lld\n", nsec10);}
    return;
  }

  uint64 dur = 1;
  // One initial word plus 8 chars per word
  uint64 len = ((strlen(name) + 7) >> 3) + 1;
  event = (event & 0xF0F) | (len << 4);		// Set name length

  fprintf(f, "%lld %lld %lld %d %s\n", nsec10, dur, event, argall, name);
  // Also put the name at the very front of the sorted event list
  fprintf(f, "%lld %lld %lld %d %s\n", -1ll, dur, event, argall, name);
}

// time dur event cpu  pid rpc  arg retval IPC name(event)
void OutputEvent(FILE* f, 
                 uint64 nsec10, uint64 duration, uint64 event, uint64 current_cpu,
                 uint64 pid, uint64 rpc, 
                 uint64 arg, uint64 retval, int ipc, const char* name) {
  // Avoid crazy big times
  bool fail = false;
  if (nsec10 >= 99900000000LL) {fail = true;}
  if (duration >= 99900000000LL) {fail = true;}
  if (nsec10 + duration >= 99900000000LL) {fail = true;}
  if (fail) {
    if (verbose) {fprintf(stdout, "BUG %lld %lld\n", nsec10, duration);}
    return;
  }

  fprintf(f, "%lld %lld %lld %lld  %lld %lld  %lld %lld %d %s (%llx)\n", 
          nsec10, duration, event, current_cpu, 
          pid, rpc, 
          arg, retval, ipc, name, event);
}

// Add the pid#/rpc#/etc. to the end of name, if not already there
string AppendNum(const string& name, uint64 num) {
  char num_temp[24];
  sprintf(num_temp, ".%lld", num & 0xffff);
  if (strstr(name.c_str(), num_temp) == NULL) {
    return name + string(num_temp);
  }
  return name;
}

// Add the pkt hash, etc. in hex to the end of name, if not already there
string AppendHexNum(const string& name, uint64 num) {
  char num_temp[24];
  sprintf(num_temp, ".%04llX", num & 0xffff);
  if (strstr(name.c_str(), num_temp) == NULL) {
    return name + string(num_temp);
  }
  return name;
}
 
// Change spaces and control codes to underscore
// Get rid of any high bits in names
string MakeSafeAscii(string s) {
  for (int i = 0; i < s.length(); ++i) {
    if (s[i] <= 0x20) {s[i] = '_';}
    if (s[i] == '"') {s[i] = '_';}
    if (s[i] == '\\') {s[i] = '_';}
    s[i] &= 0x7f;
  }
  return s;
}

bool Digit(char c) {return ('0' <= c) & (c <= '9');}

// Remove spaces from names because later freads stop at space
string ReduceSpaces(string s) {
  int k = 1;
  int len = s.length();
  if (len < 3) {return s;}
  // The very first character is unchanged
  for (int i = 1; i < len - 1; ++i) {
    if (s[i] != ' ') {
      s[k++] = s[i];
    } else {
      // Keep space (as underscore) only if between two digits
      if (Digit(s[i - 1]) && Digit(s[i + 1])) {
        s[k++] = '_';
      }
      // Else drop the space
    }
  }
  s[k++] = s[len - 1];	// The very last character
  return s.substr(0, k);
}

// FreeBSD has thread numbers 100000..165535 in block headers. Subtract 100000.
uint32 RemapHighPid(uint32 fbpid) {
  return (fbpid >= 100000) ? fbpid - 100000 : fbpid;
}

// FreeBSD and perhaps others have multiple idle threads.
// Remap all these to canonical PID 0 and remember them for updating context switches
void FixupIdlePid(uint64* pid, char* pidname, U64set* idle_pids) {
//VERYTEMP
if (keep_idle) {return;}
      // FreeBSD has multiple idle threads named idle:xxx, with different PID numbers
      // Map all of these to pid 0  
      if (memcmp(pidname , "idle:", 5) == 0) {
        idle_pids->insert(*pid);
        *pid = 0;
      }
      // Map pid 0 name to "-idle-" 
      if (*pid == 0) {strcpy(pidname, kIdleName);}
}

// Change any pids that were originally "idle:" to canonical idle pid 0
uint64 RemapIdlePid(uint64 fbpid, const U64set* idle_pids) {
  return ( (idle_pids->find(fbpid) != idle_pids->end()) ? 0 : fbpid );
}


//   +-------+-----------------------+-------------------------------+
//   | cpu#  |                  cycle counter                        | 0 module
//   +-------+-----------------------+-------------------------------+
//   | flags |                  gettimeofday                         | 1 DoDump
//   +-------------------------------+-------------------------------+
//   |                      start cycle counter                      | 2 DoDump
//   +-------------------------------+-------------------------------+
//   |                      start gettimeofday                       | 3 DoDump
//   +-------------------------------+-------------------------------+
//   |                       stop cycle counter                      | 4 DoDump
//   +-------------------------------+-------------------------------+
//   |                       stop gettimeofday                       | 5 DoDump
//   +-------------------------------+-------------------------------+
//   |                          u n u s e d                          | 6
//   +-------------------------------+-------------------------------+
//   |                          u n u s e d                          | 7
//   +-------------------------------+-------------------------------+

// If very first block, pick out time conversion parameters
// First block has extra time fields. We do sanity checking here.
bool handle_very_first_block (uint64* traceblock, uint64* base_usec_timestamp, CyclesToUsecParams* params) {
      int64 start_counts = traceblock[2];
      int64 start_usec = traceblock[3];
      int64 stop_counts = traceblock[4];
      int64 stop_usec = traceblock[5];
      uint64 base_minute_usec, base_minute_cycle, base_minute_shift;

      *base_usec_timestamp = start_usec;
      bool fail = false;
      
      int64 delta_counts = stop_counts - start_counts;
      int64 delta_usec = stop_usec - start_usec;
      if (delta_usec <= 0) {delta_usec = 1;}	// Avoid zdiv
      double counts_per_usec = (delta_counts * 1.0) / delta_usec;

      // We have some possible fixups to do on the time counter values
      // riscv can have 1MHz counts that fit in 32 bits
      // RPi arm-32 can have 54MHz counts that are truncated to 32 bits
      bool has_32bit_counts = ((start_counts | stop_counts) & 0xffffffff00000000llu) == 0;
      bool likely_rpi =   (memmem((uint8*)traceblock, 256, "Raspberry", 9) != NULL);
      bool likely_riscv = (memmem((uint8*)traceblock, 256, "u74-mc", 6) != NULL);

      // Risc-v fixup: u74 chip sometimes sets bogus bit<32> in stop cycles, so 4GB too large
      if (likely_riscv) {
        //fprintf(stderr, "likely riscv\n");
        //fprintf(stderr, "start_counts %016llx\n", start_counts);
        //fprintf(stderr, "stop_counts  %016llx\n", stop_counts);
        if ((counts_per_usec > 100.1) && 
            ((start_counts >> 32) == 0) &&
            ((stop_counts >> 32) == 1) ) {
          stop_counts &= 0x00000000FFFFFFFFLL;
          delta_counts = stop_counts - start_counts;
          counts_per_usec = (delta_counts * 1.0) / delta_usec;
          // fprintf(stderr, "counts per usec %3.1f MHz\n", counts_per_usec);
          fprintf(stderr, "rawtoevent: RISC-V fixup done.\n");
        }
      }

      // Arm-32 fixup:
      // For Arm-32, the "cycle" counter is only 32 bits at 54 MHz, so wraps
      // about every 79 seconds. This can leave stop_counts small by a few
      // multiples of 4G. We do a fix here for exactly 54 MHz. Later, we could 
      // find or take as input a different approximate counter frequency.
      // These traces likely are missing the Raspberry model name
      if (has_32bit_counts && !likely_riscv) {
        //fprintf(stderr, "likely arm-32\n");
        //fprintf(stderr, "rawtoevent: has_32bit_counts\n");
        uint64 elapsed_usec = (uint64)(delta_usec);
        uint64 elapsed_counts = (uint64)(delta_counts);
        uint64 expected_counts = elapsed_usec * mhz_32bit_counts;
        // Pick off the high bits
        uint64 approx_hi = expected_counts & 0xffffffff00000000llu;
        // Put them in
        stop_counts |= (int64)approx_hi;
        // Cross-check and change by 1 if right at a boundary
        // and off by more than 12.5% from expected MHz
        elapsed_counts = (uint64)(stop_counts - start_counts);
        uint64 ratio = elapsed_counts / elapsed_usec;
        if (ratio > (mhz_32bit_counts + (mhz_32bit_counts >> 3))) {
          // High ratio; lower stop point
          stop_counts -= 0x0000000100000000llu;
        }
        if (ratio < (mhz_32bit_counts - (mhz_32bit_counts >> 3))) {
          // Low ratio; raise stop point
          stop_counts += 0x0000000100000000llu;
        }
        delta_counts = stop_counts - start_counts;
        counts_per_usec = (delta_counts * 1.0) / delta_usec;
        fprintf(stderr, "rawtoevent: RPi fixup done.\n");
      }
      
      // Record where we stand 
      //fprintf(stderr, "# counts per second %3.1f MHz\n", counts_per_usec);
      //fprintf(stdout, "# counts per second %3.1f MHz\n", counts_per_usec);
      if (counts_per_usec < 10.0) {
        fprintf(stderr, "rawtoevent: ... Low-resolution timestamps ...\n");
      }
      
      if (verbose || hexevent) {
        fprintf(stdout, "%% %016llx = %lldcy %lldus (%lld mod 1min)\n", 
          traceblock[2], start_counts, start_usec, start_usec % 60000000l);
        fprintf(stdout, "%% %016llx\n", traceblock[3]);
        fprintf(stdout, "%% %016llx = %lldcy %lldus (%lld mod 1min)\n", 
          traceblock[4], stop_counts, stop_usec, stop_usec % 60000000l);
        fprintf(stdout, "%% %016llx\n", traceblock[5]);
        fprintf(stdout, "%% %016llx unused\n", traceblock[6]);
        fprintf(stdout, "%% %016llx unused\n", traceblock[7]);
        fprintf(stdout, "\n");
      }

      // Now do some error checking 
      if (counts_per_usec < 0.99) {
        fprintf(stderr, "rawtoevent Fail: cycles per us %3.1f < 0.99 MHz\n", counts_per_usec);
        fail = true;
      }
      if (counts_per_usec > 100.1) {
        fprintf(stderr, "rawtoevent Fail: cycles per us %3.1f > 100.1 MHz\n", counts_per_usec);
        fail = true;
      }

      // More sanity checks. If fail, ignore this block
      int blocknumber = 0;
      if (start_counts > stop_counts) {
        fprintf(stderr, "rawtoevent FAIL: block[%d] start_cy > stop_cy %lld %lld\n", blocknumber, start_counts, stop_counts);
        fail = true;
      }
      if (start_usec > stop_usec) {
        fprintf(stderr, "rawtoevent FAIL: block[%d] start_usec > stop_usec %lld %lld\n", blocknumber, start_usec, stop_usec);
        fail = true;
      }
      if (usec_per_100_years <= start_counts) {
        fprintf(stderr, "rawtoevent FAIL: block[%d] start_counts crazy large %016llx \n", blocknumber, start_counts);
        fail = true;
      }
      if (usec_per_100_years <= stop_counts) {
        fprintf(stderr, "rawtoevent FAIL: block[%d] stop_counts crazy large %016llx \n", blocknumber, stop_counts);
        fail = true;
      }

      if (fail) {
        fprintf(stderr, "rawtoevent **** FAIL in block[0] is fatal ****\n");
        fprintf(stderr, "     %016llx %016llx\n",traceblock[0], traceblock[1]);
        exit(0);
      }

      // Map start_counts <==> start_usec
      SetParams(start_counts, start_usec, stop_counts, stop_usec, params);

      // Round usec down to multiple of 1 minute
      base_minute_usec = (start_usec / 60000000) * 60000000;  
      // Backmap base_minute_usec to cycles
      base_minute_cycle = UsecToCycles(base_minute_usec, *params);

      // Now instead map base_minute_cycle <==> 0
      SetParams10(base_minute_cycle, 0, params);

  return fail;
}

//
// Usage: rawtoevent <trace file name> [-v] [-h] [-maxblock n]
//
int main (int argc, const char** argv) {
  // Some statistics
  uint64 base_usec_timestamp;
  uint64 event_count = 0;
  uint64 lo_timestamp = 0x7FFFFFFFFFFFFFFFl;
  uint64 hi_timestamp = 0;
  U64set unique_cpus;
  U64set unique_pids;
  U64set idle_pids;
  uint64 ctx_switches = 0;
  uint64 total_marks = 0;
  uint64 events_by_type[16];		// From high nibble of eventnum
  memset(events_by_type, 0, 16 * sizeof(uint64));

  int maxblock = 999999999;
  uint64 current_cpu = 0;
  uint64 traceblock[kTraceBufSize];	// 8 bytes per trace entry
  uint8 ipcblock[kTraceBufSize];	// One byte per trace entry

  uint64 current_pid[kMAX_CPUS];	// Keep track of current PID on each of 16+ cores
  uint64 current_rpc[kMAX_CPUS]; 	// Keep track of current rpcid on each of 1+6 cores
  uint64 prior_timer_irq_nsec10[kMAX_CPUS];	// For moving PC sample start_ts back
  bool at_first_cpu_block[kMAX_CPUS];	// To special-case the initial PID of each CPU in trace
  U64toString names;			// Name keyed by PID#, RPC# etc. with high type nibble

  // Start timepair is set by DoInit
  // Stop timepair is set by DoOff
  // If start_counts is zero, we got here directly without calling DoInit, 
  // which was done in some earlier run of this program. In that case, go 
  // find the start pair as the first real trace entry in the first trace block.
  CyclesToUsecParams params;

  // Events are 0..64K-1 for everything except context switch.
  // Context switch events are 0x10000 + pid
  // Initialize idle process name, pid 0
  names[0x10000] = string(kIdleName);

  // Pick up flags
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "-v") == 0) {verbose = true;}
    if (strcmp(argv[i], "-h") == 0) {hexevent = true;}
    if ((strcmp(argv[i], "-maxblock") == 0) && (i < (argc - 1))) {
      ++i;
      maxblock = atoi(argv[i]);
    }
  }
  
  for (int i = 0; i < kMAX_CPUS; ++i) {
    current_pid[i] = 0; 
    current_rpc[i] = 0;
    prior_timer_irq_nsec10[i] = 0;
    at_first_cpu_block[i] = true;
  }

  // For converting cycle counts to multiples of 100ns
  double m = kDefaultSlope;

  FILE* f = stdin;
  if (argc >= 2) {
    f = fopen(argv[1], "rb");
    if (f == NULL) {
      fprintf(stderr, "rawtoevent: %s did not open\n", argv[1]);
      exit(0);
    }
  }

  int blocknumber = 0;
  uint64 base_minute_usec, base_minute_cycle, base_minute_shift;

  // Need this to sort in front of allthe timestamps
  fprintf(stdout, "# ## VERSION: %d\n", kRawVersionNumber);
  uint8 all_flags = 0;	// They should all be the same
  uint8 first_flags;	// Just first block has tracefile version number


  //--------------------------------------------------------------------------//
  // Outer loop over blocks                                                   //
  //--------------------------------------------------------------------------//
  while (fread(traceblock, 1, sizeof(traceblock), f) != 0) {
    if (blocknumber >= maxblock) {break;}

    // Need first [1] line to get basetime in later steps
    // TODO: Move this to a stylized BASETIME comment
    ////fprintf(stdout, "# blocknumber %d\n", blocknumber);
    // These are stylized comments that eventtospan depends on for initial time
    fprintf(stdout, "# [0] %016llx cpu %02llx block %d\n", 
            traceblock[0],
            traceblock[0] >> 56,
            blocknumber);
    fprintf(stdout, "# [1] %s cpu %02llx flags %02llx block %d\n",
            FormatUsecDateTime(traceblock[1] & 0x00fffffffffffffful),
            traceblock[0] >> 56, 
            traceblock[1] >> 56,
            blocknumber);
    fprintf(stdout, 
            "# TS      DUR EVENT CPU PID RPC ARG0 RETVAL IPC NAME (t and dur multiples of 10ns)\n");

    if (verbose || hexevent) {
       fprintf(stdout, "%% %02llx %014llx\n", traceblock[0] >> 56, traceblock[0] & 0x00fffffffffffffful);
       fprintf(stdout, "%% %02llx %014llx\n", traceblock[1] >> 56, traceblock[1] & 0x00fffffffffffffful);
    }
//   +-------+-----------------------+-------------------------------+
//   | cpu#  |                  cycle counter                        | 0 module
//   +-------+-----------------------+-------------------------------+
//   | flags |                  gettimeofday                         | 1 DoDump
//   +-------+-----------------------+-------------------------------+

    // Pick out CPU number for this traceblock
    current_cpu = traceblock[0] >> 56;
    uint64 base_cycle = traceblock[0] & 0x00fffffffffffffful;

    // traceblock[1] has flags in top byte. 
    uint8 flags = traceblock[1] >> 56;
    uint64 gtod = traceblock[1] & 0x00fffffffffffffful;

    bool fail = false;
    if (kMAX_CPUS <= current_cpu) {
      fprintf(stderr, "rawtoevent FAIL: block[%d] CPU number %lld > max %d\n", blocknumber, current_cpu, kMAX_CPUS);
      fail = true;
    }
    // No constraints on base_cycle
    // No constraints on flags
    if (usec_per_100_years <= gtod) {
      fprintf(stderr, "rawtoevent FAIL: block[%d] gettimeofday crazy large %016llx\n", blocknumber, gtod);
      fail = true;
    }
  

    all_flags |= flags;
    bool this_block_has_ipc = (HasIPC(flags));

    // For each 64KB traceblock that has IPC_Flag set, also read the IPC bytes
    if (this_block_has_ipc) {
      // Extract 8KB IPC block
      int n = fread(ipcblock, 1, sizeof(ipcblock), f);
    } else {
      memset(ipcblock, 0, sizeof(ipcblock));	// Default if no IPC data
    }

// WRAPAROUND PROBLEM:
// We pick base_minute_usec here in block 0, but it can be
// long before the real wrapped trace entries in blocks 1..N
// Our downstream display does badly with seconds much over 120...
//
// We would like the base_minute_usec to be set by the first real entry in block 1 instead...
// Can still use paramaters here for basic time conversion. 
// Not much issue with overflow, I think.
//

    // If very first block, pick out time conversion parameters
    int first_real_entry = 2;
    bool very_first_block = (blocknumber == 0);
    if (very_first_block) {
      first_real_entry = 8;
      first_flags = flags;
      fail |= handle_very_first_block(traceblock, &base_usec_timestamp, &params);
    }

    if (fail) {
      fprintf(stderr, "rawtoevent **** FAIL -- skipping block[%d] ****\n", blocknumber);
      fprintf(stderr, "     %016llx %016llx\n",traceblock[0], traceblock[1]);
      for (int i = 0; i < 16; ++i) {fprintf(stderr, "  [%d] %016llx\n", i, traceblock[i]);}
      ++blocknumber;
      continue;
    }

    // Pick out CPU number for this traceblock
    current_cpu = traceblock[0] >> 56;
    unique_cpus.insert(current_cpu);	// stats

    // Pick out times for converting to 100Mhz
    uint64 prepend = base_cycle & ~0xfffff;

    // The base cycle count for this block may well be a bit later than the truncated time
    // in the first real entry, and may have wrapped in its low 20 bits. If so, the high bits 
    // we want to prepend should be one smaller.
    uint64 first_timestamp = traceblock[first_real_entry] >> 44;
    uint64 prior_t = first_timestamp;

    // If wraparound trace and in very_first_block, suppress everything except name entries
    // and hardware description
    bool keep_just_names = HasWraparound(first_flags) && very_first_block;

// Every block has PID and pidname at the front                          created by
//   +-------+-----------------------+-------------------------------+
//   | cpu#  |                  cycle counter                        | 0 module
//   +-------+-----------------------+-------------------------------+
//   | flags |                  gettimeofday                         | 1 DoDump
//   ~                                                               ~
//   +-------------------------------+-------------------------------+
//   |           (freq)              |            PID                | 2 or 8  module
//   +-------------------------------+-------------------------------+
//   |                          u n u s e d                          | 3 or 9  module
//   +-------------------------------+-------------------------------+
//   |                                                               | 4 or 10 module
//   +                            pidname                            +
//   |                                                               | 5 or 11 module
//   +-------------------------------+-------------------------------+

    if (TracefileVersion(first_flags) >= 3) {
      /* Every block has PID and pidname at the front */
      /* CPU frequency may be in the first block per CPU, in the high half of pid */
      uint64 pid = traceblock[first_real_entry + 0] & 0x00000000ffffffffLLU;
      uint64 freq_mhz = traceblock[first_real_entry + 0] >> 32;
      uint64 unused = traceblock[first_real_entry + 1];
      char pidname[24];
      pid = RemapHighPid(pid);
      memcpy(pidname, reinterpret_cast<char*>(&traceblock[first_real_entry + 2]), 16);
      pidname[16] = '\0';
      
      // FreeBSD has multiple idle threads named idle:xxx, with different PID numbers
      // Map all of these to pid 0 name -idle-, remembering them
      FixupIdlePid(&pid, pidname, &idle_pids);

      if (verbose || hexevent) {
        if (at_first_cpu_block[current_cpu]) {
          fprintf(stderr, "rawtoevent block[%d] cpu %lld pid %lld freq %lld %s\n", 
                  blocknumber, current_cpu, pid, freq_mhz, pidname);
        }
        fprintf(stdout, "%% %016llx pid %lld\n", traceblock[first_real_entry + 0], pid);
        fprintf(stdout, "%% %016llx unused\n",  traceblock[first_real_entry + 1]);
        fprintf(stdout, "%% %016llx name %s\n", traceblock[first_real_entry + 2], pidname);
        fprintf(stdout, "%% %016llx name\n",    traceblock[first_real_entry + 3]);
        fprintf(stdout, "\n");
      }

      // Remember the name for this pid
      uint64 nameinsert = PidToEvent(pid);
      string name = MakeSafeAscii(ReduceSpaces(string(pidname)));
      names[nameinsert] = name;
      
      // To allow updates of the reconstruction stack in eventtospan
      uint64 nsec10 = CyclesToNsec10(base_cycle, params);
      OutputName(stdout, nsec10, KUTRACE_PIDNAME, pid, name.c_str());

      // New user-mode process id, pid
      unique_pids.insert(pid);	// stats
      if (current_pid[current_cpu] != pid) {++ctx_switches;}	// stats
      current_pid[current_cpu] = pid;

      uint64 event = KUTRACE_USERPID;	// Context switch
      uint64 duration = 1;
      if (!keep_just_names) {
        name = AppendNum(name, pid);	// foo.12345

        // NOTE: OutputEvent here is likely a bug. Forcing a context switch at block boundary
        // unfortunately has a later timestamp than the very first entry of the block
        // because that entry's time was captured first, then reserve space which
        // switches blocks and grabs a new time for the block PID, ~300ns later than
        // the entry that is then going to be first-in-block. Hmmm.
        // The effect is that first-entry = ctx switch gets LOST.
        // Commenting out for the time being. dsites 2020.11.12. Fixes reconstruct bug.
        //
        // A possible alternate design is to back up the timestamp here to just before the 
        // first real entry.
        //
        /////OutputEvent(stdout, nsec10, duration, event, current_cpu, 
        ////            pid, 0,  0, 0, 0, name.c_str());

        // Statistics: don't count as a context switch -- almost surely same

        // dsites 2021.07.26
        // Output the very first block's context switch to the running process at trace startup
        // dsites 2021.10.20 Output initial CPU frequency if nonzero
        if (at_first_cpu_block[current_cpu]) {
          at_first_cpu_block[current_cpu] = false;
          OutputEvent(stdout, nsec10, duration, KUTRACE_USERPID, current_cpu, 
                      pid, 0,  0, 0, 0, name.c_str());
          if (0 < freq_mhz) {
            OutputEvent(stdout, nsec10, duration, KUTRACE_PSTATE, current_cpu, 
                        pid, 0,  freq_mhz, 0, 0, "-freq-");
           }
        }
      }

      first_real_entry += 4;
    }	// End of each block preprocessing


    // We wrapped if high bit of first_timestamp is 1 and high bit of base is 0
    if (Wrapped(first_timestamp, base_cycle)) {
      prepend -= 0x100000; 
      if (TRACEWRAP) {fprintf(stdout, "  Wrap0 %05llx %05llx\n", first_timestamp, base_cycle);}
    }

    //------------------------------------------------------------------------//
    // Inner loop over eight-byte entries                                     //
    //------------------------------------------------------------------------//
    for (int i = first_real_entry; i < kTraceBufSize; ++i) {
      int entry_i = i;		// Always the first word, even if i subsequently incremented
      bool has_arg = false;	// Set true if low 32 bits are used
      bool extra_word = false;	// Set true if entry is at least two words
      bool deferred_rpcid0 = false;
      uint8 ipc = ipcblock[i];

      // Completely skip any all-zero NOP entries
      if (traceblock[i] == 0LLU) {continue;}

      // Skip the entire rest of the block if all-ones entry found
      if (traceblock[i] == 0xffffffffffffffffLLU) {break;}

      // +-------------------+-----------+---------------+-------+-------+
      // | timestamp         | event     | delta | retval|      arg0     |
      // +-------------------+-----------+---------------+-------+-------+
      //          20              12         8       8           16 
      
      uint64 t = traceblock[i] >> 44;			// Timestamp
      uint64 n = (traceblock[i] >> 32) & 0xfff;		// event number
      uint64 arg    = traceblock[i] & 0x0000ffff;	// syscall/ret arg/retval
      uint64 argall = traceblock[i] & 0xffffffff;	// mark_a/b/c/d, etc.
      uint64 arg_hi = (traceblock[i] >> 16) & 0xffff;	// rx_pkt tx_pkt lglen8
      uint64 delta_t = (traceblock[i] >> 24) & 0xff;	// Opt syscall return timestamp
      uint64 retval = (traceblock[i] >> 16) & 0xff;	// Opt syscall retval

      // Completely skip any mostly-FFFF entries, but keep FFF return of 32-bit -sched-
      if ((t == 0xFFFFF) && (n == 0xFFF)) {continue;}

      // Sign extend optimized retval [-128..127] from 8 bits to 16
      retval = (uint64)(((int64)(retval << 56)) >> 56) & 0xffff;
      if (verbose) {
        fprintf(stdout, 
                "%% [%d,%d] %05llx %03llx %04llx %04llx = %lld %lld %lld, %lld %lld %02x\n", 
                blocknumber, i,
                (traceblock[i] >> 44) & 0xFFFFF, 
                (traceblock[i] >> 32) & 0xFFF, 
                (traceblock[i] >> 16) & 0xFFFF, 
                (traceblock[i] >> 0) & 0xFFFF, 
		t, n, delta_t, retval, arg, ipc);
      }

      if (is_mark(n)) {
        ++total_marks;	// stats
      } else {
        ++events_by_type[n >> 8];	// stats
      }

      uint64 event;
      if (is_contextswitch(n)) {	// Context switch
        has_arg = true;
        // Change event to new process id + 64k
        event = PidToEvent(arg);
      } else {
        // Anything else 0..64K-1
        event = n;
      }

      // 2019.03.18 Go back to preserving KUTRACE_USERPID for eventtospan
      event = n;

 
      // Module does
      // delta_cycles = now - tb->prior_cycles;
      // but records just the low 20 bits of now	
      // We have to figure out here how to account for the low 20 bits wrapping:
      // prior = ppp.f8938 now = nnn.d6f66 delta = 001.de62e
      // prior + delta_lo = ppq.d6ff6 but (ppq.d6ff6 - ppp.f8938 fits in 20 bits, so would not have generated a tsdelta
      // prior + delta = ppr.d6ff6
 

/*
 * In recording a trace event, it is possible for an interrupt to happen after 
 * KUtrace code takes the event timestamp and before it claims the storage location.
 * In this case, the interupt handling will recursively record several events 
 * before returning to the original KUtrace path, which then claims a location
 * and stores the original event with its earlier timestamp. This is called a
 * "late store." When that happens, the reconstruciton in rawtoevent needs to
 * decide whether time went forward by almost the entire 20-bit wraparound
 * period, or went backward by some amount.
 * 
 * To resolve this ambiguity, we declare that a time gap of 7/8 of the wraparound
 * period is forward time and the high 1/8 is backward time associated with an
 * otherwise undetectable backward time.
 * 
 * To mark forward time in that 1/8 (and above), we add a TSDELTA entry to the
 * trace. The exact compare for late store must be identical in kutrace_mod.c 
 * and in rawtoevent.cc.
 * 
 */

      // If TSDELTA entry, increment the prepend value.
      // argall has the time difference between this entry and previous one,
      // in units of timestamp ticks (10-20nsec).
      // If time goes backward a little, difference will be large,  otherwise it will 
      // be a small number of millions.
      // A threshold of 2,000,000,000 is good for separating large, which we ignore
      if (n == KUTRACE_TSDELTA) {
        if (argall < kLargeTsdelta) {
          // Increment time by delta
          uint64 oldfull = (prepend | prior_t);	// Old prepend old t
          uint64 newfull = oldfull + argall;

          prepend = newfull & ~0xfffffLLU;
          t =       newfull &  0xfffffLLU;
          prior_t = t;
        } else {
          // Negative TSDELTA. Do unsigned subtraction.
          uint64 oldfull = (prepend | prior_t);	// Old prepend old t
          uint64 newfull = oldfull + (0xFFFFFFFF00000000LLU | argall); // sign extend arg

          // Use newfull, but do not update prepend. Doing so would ... 
          prepend = newfull & ~0xfffffLLU;
          t =       newfull &  0xfffffLLU;
          prior_t = t;
        }
        continue;	// Skip everything else about the TSDELTA event

      } else {
        // Increment the prepend if truncated time rolls over and not caused by a late store
        if (Wrapped(prior_t, t) && !LateStore(prior_t, t)) {
          prepend += 0x100000;
        }
      }

      
      // tfull is increments of cycles from the base minute for this trace
      uint64 tfull = prepend | t;
      prior_t = t;

      // nsec10 is increments of 10ns from the base minute.
      // For a trace starting at 50 seconds into a minute and spanning 99 seconds, 
      // this reaches 14,900,000,000 which means the 
      // base minute + 149.000 000 00 seconds. More than 32 bits.
      uint64 nsec10 = CyclesToNsec10(tfull, params);
      uint64 duration = 0;

      if (has_rpcid(n)) {
        // Working on this RPC until one with arg=0
        has_arg = true;
        // Defer switching to zero until after the OutputEvent
        if (arg != 0) {current_rpc[current_cpu] = arg;}
        else {deferred_rpcid0 = true;}
      }

      // Pick out any name definitions 
      if (is_namedef(n)) {
        has_arg = true;
        // We have a name or other variable-length entry
        // Remap the raw numbering to unique ranges in names[]
        uint64 nameinsert;
        uint64 rpcid;
        uint8 lglen8;
        if (is_pidnamedef(n)) {
          nameinsert = PidToEvent(arg); 	  // Processes 0..64K, idle fixup below
        } else if (is_locknamedef(n)) {
          nameinsert = arg | 0x20000;		  // Lock names
        } else if (is_methodnamedef(n)) {
          rpcid = arg & 0xffff;			  // RPC method names
          lglen8 = arg_hi;	  		  //  may include TenLg msg len
          nameinsert = rpcid | 0x30000;
        } else if (is_kernelnamedef(n)) {
          nameinsert = arg | 0x40000;		  // Kernel version
        } else if (is_modelnamedef(n)) {
          nameinsert = arg | 0x50000;		  // CPU model
        } else if (is_hostnamedef(n)) {
          nameinsert = arg | 0x60000;		  // CPU host name
        } else if (is_queuenamedef(n)) {
          nameinsert = arg | 0x70000;		  // Queue name
        } else if (is_resnamedef(n)) {
          nameinsert = arg | 0x80000;		  // Resource name
        } else {
          nameinsert = ((n & 0x00f) << 8) | arg;  // Syscall, etc. Include type of name
        }

        char tempstring[64];
        int len = (n >> 4) & 0x00f;
        if ((len < 1) || (8 < len)) {continue;}
        // Ignore any timepair but keep the names
        if (!is_timepair(n)) {
          memset(tempstring, 0, 64);
          memcpy(tempstring, &traceblock[i + 1], (len - 1) * 8);
          
          if (is_pidnamedef(n)) {
            // FreeBSD has multiple idle threads named idle:xxx, with different PID numbers
            // Map all of these to pid 0 name -idle-, remembering them
            FixupIdlePid(&arg, tempstring, &idle_pids);
            nameinsert = PidToEvent(arg); 	  // Processes 0..64K
          }
          
          // Remember the name, except throw away the empty name
          string name = string(tempstring);
          if (is_modelnamedef(n)) {
            is_low_res_ts = (name.find("u74-mc") != 0);
          }
          name = ReduceSpaces(name);
          name = MakeSafeAscii(name);
          if (!name.empty()) {
            names[nameinsert] = name;
            ////OutputName(stdout, nsec10, nameinsert, argall, name.c_str());
            OutputName(stdout, nsec10, n, argall, name.c_str());
          }
          // Remember which event number is local_timer (or local_timer_vector) and which is -sched-
          // (these vary in different historical traces)
          if (memcmp(tempstring, "local_timer", 11) == 0) {
            gTIMER_IRQ_EVENT = KUTRACE_IRQ | (arg & 0xffff);
//fprintf(stderr, "local_timer irq = %03x %d\n", gTIMER_IRQ_EVENT, gTIMER_IRQ_EVENT);
          }
          if (memcmp(tempstring, "-sched-", 7) == 0) {
            gSCHED_EVENT = KUTRACE_SYSCALL64 | (kutrace_map_nr(arg & 0xffff));
//fprintf(stderr, "-sched- syscall = %03x %d\n", gSCHED_EVENT, gSCHED_EVENT);
          }
        }
        i += (len - 1);	// Skip over the rest of the name event
        extra_word = true;
        continue;
      }
      
      if (is_cpu_description(n)) {	// Just pass it on to eventtospan
        OutputEvent(stdout, nsec10, 1, event, current_cpu, 
                    0, 0, argall, 0, 0, "");
      }

      if (keep_just_names) {continue;}

      //========================================================================
      // Name definitions above skip this code, so do not affect lo/hi 
      if (lo_timestamp > nsec10) {lo_timestamp = nsec10;}	// stats
      if (hi_timestamp < nsec10) {hi_timestamp = nsec10;}	// stats

      // Look for new user-mode process id, pid
      if (is_contextswitch(n)) {
        has_arg = true;
        arg = RemapIdlePid(arg, &idle_pids);
        unique_pids.insert(arg);	// stats
        if (current_pid[current_cpu] != arg) {++ctx_switches;}	// stats
        current_pid[current_cpu] = arg;
      }

      // Nothing else, so dump in decimal
      // Here n is the original 12-bit event; event is (pid | 64K) if n is user-mode code
      string name = string("");

      // Put in name of event
      if (is_return(n)) {
        uint64 call_event = event & ~0x0200;
        if (names.find(call_event) != names.end()) {name.append("/" + names[call_event]);}
      } else {
        if (names.find(event) != names.end()) {name.append(names[event]);}
      }

      if (is_contextswitch(n)) {
        has_arg = true;
        uint64 target = PidToEvent(arg);
        if (names.find(target) != names.end()) {name.append(names[target]);}
        name = AppendNum(name, arg);
     }

      if (is_usermode(event)) {
        if (names.find(event) != names.end()) {name.append(names[event]);}
        name = AppendNum(name, EventToPid(event));
      }

      // If this is an optimized call, pick out the duration and leave return value
      // The ipc value for this is two 4-bit values:
      //   low bits IPC before call, high bits IPC within call
      if (is_opt_call(n, delta_t)) {
        has_arg = true;
        // Optimized call with delta_t and retval
        duration = CyclesToNsec10(tfull + delta_t, params) - nsec10;
        if (is_low_res_ts && (delta_t == 1)) {
          duration = kDefaultLowResNsec10;
        }
        if (duration == 0) {duration = 1;}	// We enforce here a minimum duration of 10ns
      } else {
        retval = 0;
      }

      // Remember timer interrupt start time, for PC sample fixup below
      if (is_timer_irq(n)) {
          prior_timer_irq_nsec10[current_cpu] = nsec10;
      }

      // Pick off non-standard PC values here
      //
      // Either of two forms:
      // (1) Possible future v4 with ts/event swapped
      // +-----------+---+-----------------------------------------------+
      // | event     |///|               PC                              |
      // +-----------+---+-----------------------------------------------+
      //      12       4                 48 
      // (2) Current scaffolding
      // +-------------------+-----------+---------------+-------+-------+
      // | timestamp         | event     |    zeros      |      arg0     |
      // +-------------------+-----------+---------------+-------+-------+
      // |                               PC                              |
      // +---------------------------------------------------------------+
      //                                 64 
      // Just deal with form (2) right now
      //
      // 2021.04.05 We now include the CPU frequency sample as arg0 in this entry if nonzero.
      //   Extract it as a separate KUTRACE_PSTATE event.
      //   Strictly speaking, the event number for PC_TEMP should be 0x121 to signify 
      //   two words, but it is in fact just 0x101.
      // 
      if (is_pc_sample(n)) {
        has_arg = true;
        extra_word = true;
        uint64 freq_mhz = arg;
        uint64 pc_sample = traceblock[++i];	// Consume second word, the PC sample
        // Change PC_TEMP to either kernel or user sample address
        event = n = (pc_sample & 0x8000000000000000LLU) ? KUTRACE_PC_K : KUTRACE_PC_U;

        // The PC sample is generated after the local_timer interrupt, but we really 
        // want its sample time to be just before that interrupt. We move it back here.
        if (prior_timer_irq_nsec10[current_cpu] != 0) {
          nsec10 = prior_timer_irq_nsec10[current_cpu] - 1;	// 10 nsec before timer IRQ
        }
        // Put a hash of the PC name into arg, so HTML display can choose colors quickly
        arg = (pc_sample >> 6) & 0xFFFF;	// Initial hash just uses PC bits <21:6>
						// This is used for drawing color
						// If addrtoline is used later, reset arg
        retval = 0;
        ipc = 0; 
        char temp_hex[24];
        sprintf(temp_hex, "PC=%012llx", pc_sample);	// Normally 48-bit PC
        name = string(temp_hex); 

        // Output the frequency event first if nonzero
        if (0 < freq_mhz) { 
          OutputEvent(stdout, nsec10, 1, KUTRACE_PSTATE, current_cpu, 
                      current_pid[current_cpu], current_rpc[current_cpu], 
                      freq_mhz, 0, 0, "-freq-");
          ++event_count;	// stats
        }
      }

      // If this is a special event marker, keep the name and arg
      if (is_special(n)) {
        has_arg = true;
        name.append(string(kSpecialName[n & 0x001f]));
        if (has_rpcid(n)) {
          name = AppendNum(names[arg | 0x30000], arg);	// method.rpcid
        } else if (is_lock(n)) {
          name = string(kSpecialName[n & 0x001f]) + names[arg | 0x20000];  // try_lockname etc.
        } else if (is_raw_pkt_hash(n)  || is_user_msg_hash(n)) {
          uint64 hash16 = ((argall >> 16) ^ argall) & 0xffffLLU;	// HTML shows this 16-bit hash
          name = AppendHexNum(name, hash16);
        } else if (n == KUTRACE_RUNNABLE) {
          // Include which PID is being made runnable, from arg
          name = AppendNum(name, arg);
        }
        if (duration == 0) {duration = 1;}	// We enforce here a minimum duration of 10ns
      }

      // If this is an unoptimized return, move the arg value to retval
      if (is_return(n)) {
        has_arg = true;
        retval = arg;
        arg = 0;
      }

      // If this is a call to an irq bottom half routine, name it BH:something
      if (is_bottom_half(n)) {
        has_arg = true;
        name.append(":");
        name.append(string(soft_irq_name[arg & 0x000f]));
      }

      // If this is a packet rx or tx, remember the time
      // Step (1) of RPC-to-packet correlation
      // NOTE: the hash stored in KUTRACE_RX_PKT KUTRACE_TX_PKT is 32 bits
      // Convention: hash16 is always shown in hex caps. Other numbers in decimal
      if (is_raw_pkt_hash(n) || is_user_msg_hash(n)) {
        arg = argall;	// Retain all 32 bits in output
      }

      // If this packet is an RPC processing start, look to create the message span
      // arg is the rpcid and arg_hi is the 16-bit packet-beginning hash
      // Step (3) of RPC-to-packet correlation
      if (is_rpc_msg(n) && (arg != 0)) {
        arg = argall;	// Retain all 32 bits in output
      }

      // MARK_A,B,C arg is six base-40 chars NUL, A_Z, 0-9, . - /
      // MARK_D     arg is unsigned int
      // +-------------------+-----------+-------------------------------+
      // | timestamp         | event     |              arg              |
      // +-------------------+-----------+-------------------------------+
      //          20              12                    32 
      if (is_mark_abc(n)) {
        has_arg = true;
        // Include the marker label string, from all 32 bits af argument
        arg = argall;	// Retain all 32 bits in output
        name += "=";
        char temp[8];
        name += Base40ToChar(arg, temp);
      }

      // Debug output. Raw 64-bit event in hex
      if (hexevent) {
        fprintf(stdout, "%05llx.%03llx ", 
          (traceblock[entry_i] >> 44) & 0xFFFFF, 
          (traceblock[entry_i] >> 32) & 0xFFF);
        if (has_arg) {
          fprintf(stdout, " %04llx%04llx ", 
            (traceblock[entry_i] >> 16) & 0xFFFF, 
            (traceblock[entry_i] >> 0) & 0xFFFF);
        } else {
          fprintf(stdout, "          "); 
        }
      }

      // If we have an empty name in the first 4K event numbers, create one
      if (name.empty() && (event <= 0xFFF) && (0 <= event)) {
        char temp[16];
        int nummask = (0x800 <= event) ? 0x1FF : 0x0FF;
        sprintf(temp, "%s%lld", missingeventname[event >> 8], event & nummask);
        // If event is syscall/ret 511 and no name, then we have a trace file
        // using 511 for -sched- mismatched with a more recent kutrace_control_names.h
        // Fix them right here
        if (event == 0x9ff) {strcpy(temp, "-sched-");} 
        if (event == 0xdff) {strcpy(temp, "-sched-");} 
        if (event == 0xbff) {strcpy(temp, "/-sched-");} 
        if (event == 0xfff) {strcpy(temp, "/-sched-");} 
        name = string(temp);
      }

      // Output the trace event
      // Output format:
      // time dur event cpu  pid rpc  arg retval IPC name(event)
      OutputEvent(stdout, nsec10, duration, event, current_cpu, 
                  current_pid[current_cpu], current_rpc[current_cpu], 
                  arg, retval, ipc, name.c_str());
      // Update some statistics
      ++event_count;	// stats

      if (hexevent && extra_word) {
        fprintf(stdout, "   %16llx\n", traceblock[entry_i + 1]); 
      }

      // Do deferred switch to rpcid = 0
      if (deferred_rpcid0) {current_rpc[current_cpu] = 0;}

    }
    //------------------------------------------------------------------------//
    // End inner loop over eight-byte entries                                 //
    //------------------------------------------------------------------------//

    ++blocknumber;

  }	// while (fread...
  //--------------------------------------------------------------------------//
  // End outer loop over blocks                                               //
  //--------------------------------------------------------------------------//


  fclose(f);

  // Pass along the OR of all incoming raw traceblock flags, in particular IPC_Flag 
  fprintf(stdout, "# ## FLAGS: %d\n", all_flags);


  // Reduce timestamps to start at no more than 60 seconds after the base minute.
  // With wraparound tracing, we don't know the true value of lo_timestamp until
  // possibly the very last input block. So we offset here. The output file already 
  // has the larger times so eventtospan will reduce those. 
  uint64 extra_minutes = lo_timestamp / 6000000000l;
  uint64 offset_timestamp = extra_minutes * 6000000000l;
  lo_timestamp -= offset_timestamp;
  hi_timestamp -= offset_timestamp;
  double lo_seconds = lo_timestamp / 100000000.0;
  double hi_seconds = hi_timestamp / 100000000.0;
if (lo_seconds < 0.0) {fprintf(stderr,"BUG: lo_seconds < 0.0 %12.8f\n", lo_seconds);}
if (hi_seconds > 999.0) {fprintf(stderr,"BUG: hi_seconds > 999.0 %12.8f\n", hi_seconds);}
  double total_seconds = hi_seconds - lo_seconds;
  if (total_seconds <= 0.0) {
    lo_seconds = 0.0;
    hi_seconds = 1.0;
    total_seconds = 1.0;	// avoid zdiv
  }
  // Pass along the time bounds 
  fprintf(stdout, "# ## TIMES: %10.8f %10.8f\n", lo_seconds, hi_seconds);


  uint64 total_cpus = unique_cpus.size();
  if (total_cpus == 0) {total_cpus = 1;}	// avoid zdiv
   
  //fprintf(stderr, "rawtoevent(%3.1fMB):\n", blocknumber / kTraceBlocksPerMB); 
  //fprintf(stderr, 
  //        "  %s,  %lld events, %lld CPUs  (%1.0f/sec/cpu)\n",
  //        FormatSecondsDateTime(base_usec_timestamp / 1000000),
  //        event_count, total_cpus, (event_count / total_seconds) /total_cpus); 
  uint64 total_irqs  = events_by_type[5] + events_by_type[7];
  uint64 total_traps = events_by_type[4] + events_by_type[6];
  uint64 total_sys64 = events_by_type[8] + events_by_type[9] +
                       events_by_type[10] + events_by_type[11];
  uint64 total_sys32 = events_by_type[12] + events_by_type[13] +
                       events_by_type[14] + events_by_type[15];

  //fprintf(stderr, "  %lld IRQ, %lld Trap, %lld Sys64, %lld Sys32, %lld Mark\n",
  //        total_irqs, total_traps, total_sys64, total_sys32, total_marks); 
  //fprintf(stderr, "  %lld PIDs, %lld context-switches (%1.0f/sec/cpu)\n", 
  //        (u64)unique_pids.size(), ctx_switches, (ctx_switches / total_seconds) / total_cpus);

  fprintf(stderr, "rawtoevent: %llu events\n", event_count); 
  fprintf(stderr, 
          "  %5.3f elapsed seconds: %5.3f to %5.3f\n", 
          total_seconds, lo_seconds, hi_seconds); 

}

