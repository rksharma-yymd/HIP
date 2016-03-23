/*
Copyright (c) 2015-2016 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/
/**
 * @file hip_hcc.cpp
 *
 * Contains definitions for functions that are large enough that we don't want to inline them everywhere.
 * This file is compiled and linked into apps running HIP / HCC path.
 */

// HIP includes:
#define HIP_HCC
#include "hcc_detail/staging_buffer.h"
#include "hcc_detail/hip_common.h"
#include "hcc_detail/hip_util.h"
#include "hcc_detail/hip_inline.h"
// TODO, re-org header order.
extern const char *ihipErrorString(hipError_t hip_error);

// #define USE_MEMCPYTOSYMBOL
//
//Use the new HCC accelerator_view::copy instead of am_copy

class ihipException : public std::exception
{
public:
    ihipException(hipError_t e) : _code(e) {};

    hipError_t _code; 
};


const hipStream_t hipStreamNull = 0x0;

struct ihipDevice_t;


const char* ihipCommandName[] = {
    "CopyH2H", "CopyH2D", "CopyD2H", "CopyD2D", "Kernel"
};





// TODO - move async copy code into stream?  Stream->async-copy.
// Add PreCopy / PostCopy to manage locks?
//


// Internal stream structure.




//----
// Internal event structure:
enum hipEventStatus_t {
   hipEventStatusUnitialized = 0, // event is unutilized, must be "Created" before use.
   hipEventStatusCreated     = 1,
   hipEventStatusRecording   = 2, // event has been enqueued to record something.
   hipEventStatusRecorded    = 3, // event has been recorded - timestamps are valid.
} ;


// internal hip event structure.
struct ihipEvent_t {
    hipEventStatus_t       _state;

    hipStream_t           _stream;  // Stream where the event is recorded, or NULL if all streams.
    unsigned              _flags;

    hc::completion_future _marker;
    uint64_t              _timestamp;  // store timestamp, may be set on host or by marker.

    SIGSEQNUM             _copy_seq_id;
} ;





//-------------------------------------------------------------------------------------------------



//=================================================================================================
// Global Data Structures:
//=================================================================================================
//TLS - must be initialized here.
thread_local hipError_t tls_lastHipError = hipSuccess;
thread_local int tls_defaultDevice = 0;

// Global initialization.
std::once_flag hip_initialized;
ihipDevice_t *g_devices; // Array of all non-emulated (ie GPU) accelerators in the system.
bool g_visible_device = false; // Set the flag when HIP_VISIBLE_DEVICES is set
//unsigned g_deviceCnt;
std::vector<int> g_hip_visible_devices; /* vector of integers that contains the visible device IDs */
hsa_agent_t g_cpu_agent ;   // the CPU agent.
//=================================================================================================


//=================================================================================================
//Forward Declarations:
//=================================================================================================

//=================================================================================================
// Implementation:
//=================================================================================================


//=================================================================================================
// ihipSignal_t:
//=================================================================================================
//
//---
ihipSignal_t::ihipSignal_t() :  _sig_id(0)
{
    if (hsa_signal_create(0/*value*/, 0, NULL, &_hsa_signal) != HSA_STATUS_SUCCESS) {
        throw ihipException(hipErrorOutOfResources);
    }
    //tprintf (DB_SIGNAL, "  allocated hsa_signal=%lu\n", (_hsa_signal.handle));
}

//---
ihipSignal_t::~ihipSignal_t()
{
    tprintf (DB_SIGNAL, "  destroy hsa_signal #%lu (#%lu)\n", (_hsa_signal.handle), _sig_id);
    if (hsa_signal_destroy(_hsa_signal) != HSA_STATUS_SUCCESS) {
       throw ihipException(hipErrorOutOfResources);
    }
};



//=================================================================================================
// ihipStream_t:
//=================================================================================================
//---
ihipStream_t::ihipStream_t(unsigned device_index, hc::accelerator_view av, SeqNum_t id, unsigned int flags) :
    _id(id),
    _av(av),
    _flags(flags),
    _device_index(device_index),
    _last_command_type(ihipCommandCopyH2D),
    _last_copy_signal(NULL),
    _signalCursor(0),
    _stream_sig_id(0),
    _oldest_live_sig_id(1)
{
    tprintf(DB_SYNC, " streamCreate: stream=%p\n", this);
    _signalPool.resize(HIP_STREAM_SIGNALS > 0 ? HIP_STREAM_SIGNALS : 1);

};


//---
ihipStream_t::~ihipStream_t()
{
    _signalPool.clear();
}

//---
inline ihipDevice_t * ihipStream_t::getDevice() const
{
    if (ihipIsValidDevice(_device_index)) {
        return &g_devices[_device_index];
    } else {
        return NULL;
    }
};



//---
void ihipStream_t::reclaimSignals_ts(SIGSEQNUM sigNum)
{
    tprintf(DB_SIGNAL, "reclaim signal #%lu\n", sigNum);
    // Mark all signals older and including this one as available for
    _oldest_live_sig_id = sigNum+1;
}


//---
void ihipStream_t::waitCopy(ihipSignal_t *signal)
{
    hsa_signal_wait_acquire(signal->_hsa_signal, HSA_SIGNAL_CONDITION_LT, 1, UINT64_MAX, HSA_WAIT_STATE_ACTIVE);

    SIGSEQNUM sigNum = signal->_sig_id;

    tprintf(DB_SIGNAL, "waitCopy reclaim signal #%lu\n", sigNum);
    // Mark all signals older and including this one as available for reclaim
    if (sigNum > _oldest_live_sig_id) {
        _oldest_live_sig_id = sigNum+1; // TODO, +1 here seems dangerous.
    }

}


//---
//Wait for all kernel and data copy commands in this stream to complete.
void ihipStream_t::wait(bool assertQueueEmpty)
{
    if (! assertQueueEmpty) {
        tprintf (DB_SYNC, "stream %p wait for queue-empty..\n", this);
        _av.wait();
    }
    if (_last_copy_signal) {
        tprintf (DB_SYNC, "stream %p wait for lastCopy:#%lu...\n", this, _last_copy_signal ? _last_copy_signal->_sig_id: 0x0 );
        this->waitCopy(_last_copy_signal);
    }

    // Reset the stream to "empty" - next command will not set up an inpute dependency on any older signal.
    _last_command_type = ihipCommandCopyH2D;
    _last_copy_signal = NULL;
};



//---
// Allocate a new signal from the signal pool.
// Returned signals have value of 0.
// Signals are intended for use in this stream and are always reclaimed "in-order".
ihipSignal_t *ihipStream_t::allocSignal()
{
    int numToScan = _signalPool.size();
    do {
        auto thisCursor = _signalCursor;
        if (++_signalCursor == _signalPool.size()) {
            _signalCursor = 0;
        }

        if (_signalPool[thisCursor]._sig_id < _oldest_live_sig_id) {
            SIGSEQNUM oldSigId = _signalPool[thisCursor]._sig_id;
            _signalPool[thisCursor]._index = thisCursor;
            _signalPool[thisCursor]._sig_id  =  ++_stream_sig_id;  // allocate it.
            tprintf(DB_SIGNAL, "allocatSignal #%lu at pos:%i (old sigId:%lu < oldest_live:%lu)\n", 
                    _signalPool[thisCursor]._sig_id,
                    thisCursor, oldSigId, _oldest_live_sig_id);



            return &_signalPool[thisCursor];
        }

    } while (--numToScan) ;

    assert(numToScan == 0);

    // Have to grow the pool:
    _signalCursor = _signalPool.size(); // set to the beginning of the new entries:
    if (_signalCursor > 10000) {
        fprintf (stderr, "warning: signal pool size=%d, may indicate runaway number of inflight commands\n", _signalCursor);
    }
    _signalPool.resize(_signalPool.size() * 2);
    tprintf (DB_SIGNAL, "grow signal pool to %zu entries, cursor=%d\n", _signalPool.size(), _signalCursor);
    return allocSignal();  // try again,

    // Should never reach here.
    assert(0);
}


//---
void ihipStream_t::enqueueBarrier(hsa_queue_t* queue, ihipSignal_t *depSignal)
{

    // Obtain the write index for the command queue
    uint64_t index = hsa_queue_load_write_index_relaxed(queue);
    const uint32_t queueMask = queue->size - 1;

    // Define the barrier packet to be at the calculated queue index address
    hsa_barrier_and_packet_t* barrier = &(((hsa_barrier_and_packet_t*)(queue->base_address))[index&queueMask]);
    memset(barrier, 0, sizeof(hsa_barrier_and_packet_t));

    // setup header
    uint16_t header = HSA_PACKET_TYPE_BARRIER_AND << HSA_PACKET_HEADER_TYPE;
    header |= 1 << HSA_PACKET_HEADER_BARRIER;
    //header |= HSA_FENCE_SCOPE_SYSTEM << HSA_PACKET_HEADER_ACQUIRE_FENCE_SCOPE;
    //header |= HSA_FENCE_SCOPE_SYSTEM << HSA_PACKET_HEADER_RELEASE_FENCE_SCOPE;
    barrier->header = header;

    barrier->dep_signal[0] = depSignal->_hsa_signal;

    barrier->completion_signal.handle = 0;

    // TODO - check queue overflow, return error:
    // Increment write index and ring doorbell to dispatch the kernel
    hsa_queue_store_write_index_relaxed(queue, index+1);
    hsa_signal_store_relaxed(queue->doorbell_signal, index);
}


//--
//When the commands in a stream change types (ie kernel command follows a data command,
//or data command follows a kernel command), then we need to add a barrier packet
//into the stream to mimic CUDA stream semantics.  (some hardware uses separate
//queues for data commands and kernel commands, and no implicit ordering is provided).
//
inline bool ihipStream_t::preKernelCommand()
{
    _mutex.lock(); // will be unlocked in postKernelCommand

    bool addedSync = false;
    // If switching command types, we need to add a barrier packet to synchronize things.
    if (_last_command_type != ihipCommandKernel) {
        if (_last_copy_signal) {
            addedSync = true;

            hsa_queue_t * q =  (hsa_queue_t*)_av.get_hsa_queue();
            if (HIP_DISABLE_HW_KERNEL_DEP == 0) {
                this->enqueueBarrier(q, _last_copy_signal);
                tprintf (DB_SYNC, "stream %p switch %s to %s (barrier pkt inserted with wait on #%lu)\n",
                        this, ihipCommandName[_last_command_type], ihipCommandName[ihipCommandKernel], _last_copy_signal->_sig_id)

            } else if (HIP_DISABLE_HW_KERNEL_DEP>0) {
                    tprintf (DB_SYNC, "stream %p switch %s to %s (HOST wait for previous...)\n",
                            this, ihipCommandName[_last_command_type], ihipCommandName[ihipCommandKernel]);
                    this->waitCopy(_last_copy_signal);
            } else if (HIP_DISABLE_HW_KERNEL_DEP==-1) {
                tprintf (DB_SYNC, "stream %p switch %s to %s (IGNORE dependency)\n",
                        this, ihipCommandName[_last_command_type], ihipCommandName[ihipCommandKernel]);
            }
        }
        _last_command_type = ihipCommandKernel;
    }

    return addedSync;
}


//---
inline void ihipStream_t::postKernelCommand(hc::completion_future &kernelFuture)
{
    _last_kernel_future = kernelFuture;

    _mutex.unlock();
};



//---
// Called whenever a copy command is set to the stream.
// Examines the last command sent to this stream and returns a signal to wait on, if required.
int ihipStream_t::preCopyCommand(ihipSignal_t *lastCopy, hsa_signal_t *waitSignal, ihipCommand_t copyType)
{
    int needSync = 0;

    waitSignal->handle = 0;

    //_mutex.lock(); // will be unlocked in postCopyCommand

    // If switching command types, we need to add a barrier packet to synchronize things.
    if (FORCE_SAMEDIR_COPY_DEP || (_last_command_type != copyType)) {


        if (_last_command_type == ihipCommandKernel) {
            tprintf (DB_SYNC, "stream %p switch %s to %s (async copy dep on prev kernel)\n",
                    this, ihipCommandName[_last_command_type], ihipCommandName[copyType]);
            needSync = 1;
            hsa_signal_t *hsaSignal = (static_cast<hsa_signal_t*> (_last_kernel_future.get_native_handle()));
            if (hsaSignal) {
                *waitSignal = * hsaSignal;
            }
        } else if (_last_copy_signal) {
            needSync = 1;
            tprintf (DB_SYNC, "stream %p switch %s to %s (async copy dep on other copy #%lu)\n",
                    this, ihipCommandName[_last_command_type], ihipCommandName[copyType], _last_copy_signal->_sig_id);
            *waitSignal = _last_copy_signal->_hsa_signal;
        }

        if (HIP_DISABLE_HW_COPY_DEP && needSync) {
            if (HIP_DISABLE_HW_COPY_DEP == -1) {
                tprintf (DB_SYNC, "IGNORE copy dependency\n")

            } else {
                tprintf (DB_SYNC, "HOST-wait for copy dependency\n")
                // do the wait here on the host, and disable the device-side command resolution.
                hsa_signal_wait_acquire(*waitSignal, HSA_SIGNAL_CONDITION_LT, 1, UINT64_MAX, HSA_WAIT_STATE_ACTIVE);
                needSync = 0;
            }
        }

        _last_command_type = copyType;
    }

    _last_copy_signal = lastCopy;

    return needSync;
}




//=================================================================================================
//
//Reset the device - this is called from hipDeviceReset.
//Device may be reset multiple times, and may be reset after init.
void ihipDevice_t::reset()
{
    // Reset and remove streams:
    _streams.clear();

    // Reset and release all memory stored in the tracker:
    am_memtracker_reset(_acc);

};


//---
void ihipDevice_t::init(unsigned device_index, hc::accelerator acc)
{
    _stream_id = 0;

    _device_index = device_index;
    _acc = acc;
    hsa_agent_t *agent = static_cast<hsa_agent_t*> (acc.get_hsa_agent());
    if (agent) {
        int err = hsa_agent_get_info(*agent, (hsa_agent_info_t)HSA_AMD_AGENT_INFO_COMPUTE_UNIT_COUNT, &_compute_units);
        if (err != HSA_STATUS_SUCCESS) {
            _compute_units = 1;
        }

        _hsa_agent = *agent;
    } else {
        _hsa_agent.handle = static_cast<uint64_t> (-1);
    }

    getProperties(&_props);

    _default_stream = new ihipStream_t(device_index, acc.get_default_view(), _stream_id++, hipStreamDefault);
    this->_streams.push_back(_default_stream);
    tprintf(DB_SYNC, "created device with default_stream=%p\n", _default_stream);


    hsa_region_t *pinnedHostRegion;
    pinnedHostRegion = static_cast<hsa_region_t*>(_acc.get_hsa_am_system_region());
    _staging_buffer[0] = new StagingBuffer(_hsa_agent, *pinnedHostRegion, HIP_STAGING_SIZE*1024, HIP_STAGING_BUFFERS);
    _staging_buffer[1] = new StagingBuffer(_hsa_agent, *pinnedHostRegion, HIP_STAGING_SIZE*1024, HIP_STAGING_BUFFERS);

};


ihipDevice_t::~ihipDevice_t()
{
    if (_default_stream) {
        delete _default_stream;
        _default_stream = NULL;
    }

    for (int i=0; i<2; i++) {
        if (_staging_buffer[i]) {
            delete _staging_buffer[i];
            _staging_buffer[i] = NULL;
        }
    }
}

//----




//=================================================================================================
// Utility functions, these are not part of the public HIP API
//=================================================================================================

//=================================================================================================

#define DeviceErrorCheck(x) if (x != HSA_STATUS_SUCCESS) { return hipErrorInvalidDevice; }

#define ErrorCheck(x) error_check(x, __LINE__, __FILE__)

void error_check(hsa_status_t hsa_error_code, int line_num, std::string str) {
  if (hsa_error_code != HSA_STATUS_SUCCESS) {
    printf("HSA reported error!\n In file: %s\nAt line: %d\n", str.c_str(),line_num);
  }
}

hsa_status_t get_region_info(hsa_region_t region, void* data)
{
    hsa_status_t err;
    hipDeviceProp_t* p_prop = reinterpret_cast<hipDeviceProp_t*>(data);
    uint32_t region_segment;

    // Get region segment
    err = hsa_region_get_info(region, HSA_REGION_INFO_SEGMENT, &region_segment);
    ErrorCheck(err);

    switch(region_segment) {
    case HSA_REGION_SEGMENT_READONLY:
        err = hsa_region_get_info(region, HSA_REGION_INFO_SIZE, &(p_prop->totalConstMem)); break;
    /* case HSA_REGION_SEGMENT_PRIVATE:
        cout<<"PRIVATE"<<endl; private segment cannot be queried */
    case HSA_REGION_SEGMENT_GROUP:
        err = hsa_region_get_info(region, HSA_REGION_INFO_SIZE, &(p_prop->sharedMemPerBlock)); break;
    default: break;
    }
    return HSA_STATUS_SUCCESS;
}

// Determines if the given agent is of type HSA_DEVICE_TYPE_GPU and counts it.
static hsa_status_t countGpuAgents(hsa_agent_t agent, void *data) {
    if (data == NULL) {
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    }
    hsa_device_type_t device_type;
    hsa_status_t status = hsa_agent_get_info(agent, HSA_AGENT_INFO_DEVICE, &device_type);
    if (status != HSA_STATUS_SUCCESS) {
        return status;
    }
    if (device_type == HSA_DEVICE_TYPE_GPU) {
        (*static_cast<int*>(data))++;
    }
    return HSA_STATUS_SUCCESS;
}

// Internal version,
hipError_t ihipDevice_t::getProperties(hipDeviceProp_t* prop)
{
    hipError_t e = hipSuccess;
    hsa_status_t err;

    // Set some defaults in case we don't find the appropriate regions:
    prop->totalGlobalMem = 0;
    prop->totalConstMem = 0;
    prop->sharedMemPerBlock = 0;
    prop-> maxThreadsPerMultiProcessor = 0;
    prop->regsPerBlock = 0;

    if (_hsa_agent.handle == -1) {
        return hipErrorInvalidDevice;
    }

    // Iterates over the agents to determine Multiple GPU devices
    // using the countGpuAgents callback.
    //! @bug : on HCC, isMultiGpuBoard returns True if system contains multiple GPUS (rather than if GPU is on a multi-ASIC board)
    int gpuAgentsCount = 0;
    err = hsa_iterate_agents(countGpuAgents, &gpuAgentsCount);
    if (err == HSA_STATUS_INFO_BREAK) { err = HSA_STATUS_SUCCESS; }
    DeviceErrorCheck(err);
    prop->isMultiGpuBoard = 0 ? gpuAgentsCount < 2 : 1;

    // Get agent name
    err = hsa_agent_get_info(_hsa_agent, HSA_AGENT_INFO_NAME, &(prop->name));
    DeviceErrorCheck(err);

    // Get agent node
    uint32_t node;
    err = hsa_agent_get_info(_hsa_agent, HSA_AGENT_INFO_NODE, &node);
    DeviceErrorCheck(err);

    // Get wavefront size
    err = hsa_agent_get_info(_hsa_agent, HSA_AGENT_INFO_WAVEFRONT_SIZE,&prop->warpSize);
    DeviceErrorCheck(err);

    // Get max total number of work-items in a workgroup
    err = hsa_agent_get_info(_hsa_agent, HSA_AGENT_INFO_WORKGROUP_MAX_SIZE, &prop->maxThreadsPerBlock );
    DeviceErrorCheck(err);

    // Get max number of work-items of each dimension of a work-group
    uint16_t work_group_max_dim[3];
    err = hsa_agent_get_info(_hsa_agent, HSA_AGENT_INFO_WORKGROUP_MAX_DIM, work_group_max_dim);
    DeviceErrorCheck(err);
    for( int i =0; i< 3 ; i++) {
        prop->maxThreadsDim[i]= work_group_max_dim[i];
    }

    hsa_dim3_t grid_max_dim;
    err = hsa_agent_get_info(_hsa_agent, HSA_AGENT_INFO_GRID_MAX_DIM, &grid_max_dim);
    DeviceErrorCheck(err);
    prop->maxGridSize[0]= (int) ((grid_max_dim.x == UINT32_MAX) ? (INT32_MAX) : grid_max_dim.x);
    prop->maxGridSize[1]= (int) ((grid_max_dim.y == UINT32_MAX) ? (INT32_MAX) : grid_max_dim.y);
    prop->maxGridSize[2]= (int) ((grid_max_dim.z == UINT32_MAX) ? (INT32_MAX) : grid_max_dim.z);

    // Get Max clock frequency
    err = hsa_agent_get_info(_hsa_agent, (hsa_agent_info_t)HSA_AMD_AGENT_INFO_MAX_CLOCK_FREQUENCY, &prop->clockRate);
    prop->clockRate *= 1000.0;   // convert Mhz to Khz.
    DeviceErrorCheck(err);

    //uint64_t counterHz;
    //err = hsa_system_get_info(HSA_SYSTEM_INFO_TIMESTAMP_FREQUENCY, &counterHz);
    //DeviceErrorCheck(err);
    //prop->clockInstructionRate = counterHz / 1000;
    prop->clockInstructionRate = 100*1000; /* TODO-RT - hard-code until HSART has function to properly report clock */

    // Get Agent BDFID (bus/device/function ID)
    uint16_t bdf_id = 1;
    err = hsa_agent_get_info(_hsa_agent, (hsa_agent_info_t)HSA_AMD_AGENT_INFO_BDFID, &bdf_id);
    DeviceErrorCheck(err);

    // BDFID is 16bit uint: [8bit - BusID | 5bit - Device ID | 3bit - Function/DomainID]
    // TODO/Clarify: cudaDeviceProp::pciDomainID how to report?
    // prop->pciDomainID =  bdf_id & 0x7;
    prop->pciDeviceID =  (bdf_id>>3) & 0x1F;
    prop->pciBusID =  (bdf_id>>8) & 0xFF;

    // Masquerade as a 3.0-level device. This will change as more HW functions are properly supported.
    // Application code should use the arch.has* to do detailed feature detection.
    prop->major = 2;
    prop->minor = 0;

    // Get number of Compute Unit
    err = hsa_agent_get_info(_hsa_agent, (hsa_agent_info_t)HSA_AMD_AGENT_INFO_COMPUTE_UNIT_COUNT, &(prop->multiProcessorCount));
    DeviceErrorCheck(err);

    // TODO-hsart - this appears to return 0?
    uint32_t cache_size[4];
    err = hsa_agent_get_info(_hsa_agent, HSA_AGENT_INFO_CACHE_SIZE, cache_size);
    DeviceErrorCheck(err);
    prop->l2CacheSize = cache_size[1];

    /* Computemode for HSA Devices is always : cudaComputeModeDefault */
    prop->computeMode = 0;

    // Get Max Threads Per Multiprocessor
/*
    HsaSystemProperties props;
    hsaKmtReleaseSystemProperties();
    if(HSAKMT_STATUS_SUCCESS == hsaKmtAcquireSystemProperties(&props)) {
        HsaNodeProperties node_prop = {0};
        if(HSAKMT_STATUS_SUCCESS == hsaKmtGetNodeProperties(node, &node_prop)) {
            uint32_t waves_per_cu = node_prop.MaxWavesPerSIMD;
            prop-> maxThreadsPerMultiProcessor = prop->warpsize*waves_per_cu;
        }
    }
*/

    // Get memory properties
    err = hsa_agent_iterate_regions(_hsa_agent, get_region_info, prop);
    DeviceErrorCheck(err);

    // Get the size of the region we are using for Accelerator Memory allocations:
    hsa_region_t *am_region = static_cast<hsa_region_t*>(_acc.get_hsa_am_region());
    err = hsa_region_get_info(*am_region, HSA_REGION_INFO_SIZE, &prop->totalGlobalMem);
    DeviceErrorCheck(err);
    // maxSharedMemoryPerMultiProcessor should be as the same as group memory size.
    // Group memory will not be paged out, so, the physical memory size is the total shared memory size, and also equal to the group region size.
    prop->maxSharedMemoryPerMultiProcessor = prop->totalGlobalMem;

    // Get Max memory clock frequency
    //err = hsa_region_get_info(*am_region, (hsa_region_info_t)HSA_AMD_REGION_INFO_MAX_CLOCK_FREQUENCY, &prop->memoryClockRate);
    DeviceErrorCheck(err);
    prop->memoryClockRate *= 1000.0;   // convert Mhz to Khz.

    // Get global memory bus width in bits
    //err = hsa_region_get_info(*am_region, (hsa_region_info_t)HSA_AMD_REGION_INFO_BUS_WIDTH, &prop->memoryBusWidth);
    DeviceErrorCheck(err);

    // Set feature flags - these are all mandatory for HIP on HCC path:
    // Some features are under-development and future revs may support flags that are currently 0.
    // Reporting of these flags should be synchronized with the HIP_ARCH* compile-time defines in hip_runtime.h

    prop->arch.hasGlobalInt32Atomics       = 1;
    prop->arch.hasGlobalFloatAtomicExch    = 1;
    prop->arch.hasSharedInt32Atomics       = 1;
    prop->arch.hasSharedFloatAtomicExch    = 1;
    prop->arch.hasFloatAtomicAdd           = 0;
    prop->arch.hasGlobalInt64Atomics       = 1;
    prop->arch.hasSharedInt64Atomics       = 1;
    prop->arch.hasDoubles                  = 1; // TODO - true for Fiji.
    prop->arch.hasWarpVote                 = 1;
    prop->arch.hasWarpBallot               = 1;
    prop->arch.hasWarpShuffle              = 1;
    prop->arch.hasFunnelShift              = 0; // TODO-hcc
    prop->arch.hasThreadFenceSystem        = 0; // TODO-hcc
    prop->arch.hasSyncThreadsExt           = 0; // TODO-hcc
    prop->arch.hasSurfaceFuncs             = 0; // TODO-hcc
    prop->arch.has3dGrid                   = 1;
    prop->arch.hasDynamicParallelism       = 0;

    prop->concurrentKernels = 1; // All ROCR hardware supports executing multiple kernels concurrently
    return e;
}


// Implement "default" stream syncronization
//   This waits for all other streams to drain before continuing.
//   If waitOnSelf is set, this additionally waits for the default stream to empty.
void ihipDevice_t::syncDefaultStream(bool waitOnSelf)
{
    tprintf(DB_SYNC, "syncDefaultStream\n");

    for (auto streamI=_streams.begin(); streamI!=_streams.end(); streamI++) {
        ihipStream_t *stream = *streamI;
       
        // Don't wait for streams that have "opted-out" of syncing with NULL stream.
        // And - don't wait for the NULL stream
        if (!(stream->_flags & hipStreamNonBlocking)) {

            if (waitOnSelf || (stream != _default_stream)) {
                // TODO-hcc - use blocking or active wait here?
                // TODO-sync - cudaDeviceBlockingSync
                stream->wait();
            }
        }
    }
}


//---
//Heavyweight synchronization that waits on all streams, ignoring hipStreamNonBlocking flag.
void ihipDevice_t::waitAllStreams()
{
    tprintf(DB_SYNC, "waitAllStream\n");
    for (auto streamI=_streams.begin(); streamI!=_streams.end(); streamI++) {
        (*streamI)->wait();
    }
}





// Read environment variables.
void ihipReadEnv_I(int *var_ptr, const char *var_name1, const char *var_name2, const char *description)
{
    char * env = getenv(var_name1);

    // Check second name if first not defined, used to allow HIP_ or CUDA_ env vars.
    if ((env == NULL) && strcmp(var_name2, "0")) {
        env = getenv(var_name2);
    }

    // Check if the environment variable is either HIP_VISIBLE_DEVICES or CUDA_LAUNCH_BLOCKING, which
    // contains a sequence of comma-separated device IDs
    if (!(strcmp(var_name1,"HIP_VISIBLE_DEVICES") && strcmp(var_name2, "CUDA_VISIBLE_DEVICES")) && env){
        // Parse the string stream of env and store the device ids to g_hip_visible_devices global variable
        std::string str = env;
        std::istringstream ss(str);
        std::string device_id;
        // Clean up the defult value
        g_hip_visible_devices.clear();
        g_visible_device = true;
        // Read the visible device numbers
        while (std::getline(ss, device_id, ',')) {
            if (atoi(device_id.c_str()) >= 0) {
                g_hip_visible_devices.push_back(atoi(device_id.c_str()));
            }else// Any device number after invalid number will not present
                break;
        }
        // Print out the number of ids
        if (HIP_PRINT_ENV) {
            printf ("%-30s = ", var_name1);
            for(int i=0;i<g_hip_visible_devices.size();i++)
                printf ("%2d ", g_hip_visible_devices[i]);
            printf (": %s\n", description);
        }
    }
    else { // Parse environment variables with sigle value
        // Default is set when variable is initialized (at top of this file), so only override if we find
        // an environment variable.
        if (env) {
            long int v = strtol(env, NULL, 0);
            *var_ptr = (int) (v);
        }
        if (HIP_PRINT_ENV) {
            printf ("%-30s = %2d : %s\n", var_name1, *var_ptr, description);
        }
    }

}

#if defined (DEBUG)

#define READ_ENV_I(_build, _ENV_VAR, _ENV_VAR2, _description) \
    if ((_build == release) || (_build == debug) {\
        ihipReadEnv_I(&_ENV_VAR, #_ENV_VAR, #_ENV_VAR2, _description);\
    };

#else

#define READ_ENV_I(_build, _ENV_VAR, _ENV_VAR2, _description) \
    if (_build == release) {\
        ihipReadEnv_I(&_ENV_VAR, #_ENV_VAR, #_ENV_VAR2, _description);\
    };

#endif

// Determines if the given agent is of type HSA_DEVICE_TYPE_GPU and counts it.
static hsa_status_t findCpuAgent(hsa_agent_t agent, void *data) 
{
    hsa_device_type_t device_type;
    hsa_status_t status = hsa_agent_get_info(agent, HSA_AGENT_INFO_DEVICE, &device_type);
    if (status != HSA_STATUS_SUCCESS) {
        return status;
    }
    if (device_type == HSA_DEVICE_TYPE_CPU) {
        (*static_cast<hsa_agent_t*>(data)) = agent;
        return HSA_STATUS_INFO_BREAK;
    }

    return HSA_STATUS_SUCCESS;
}


//---
//Function called one-time at initialization time to construct a table of all GPU devices.
//HIP/CUDA uses integer "deviceIds" - these are indexes into this table.
//AMP maintains a table of accelerators, but some are emulated - ie for debug or CPU.
//This function creates a vector with only the GPU accelerators.
//It is called with C++11 call_once, which provided thread-safety.
void ihipInit()
{

#if COMPILE_TRACE_MARKER 
    amdtInitializeActivityLogger();
    amdtScopedMarker("ihipInit", "HIP", NULL); 
#endif
    /*
     * Environment variables
     */
    g_hip_visible_devices.push_back(0); /* Set the default value of visible devices */
    READ_ENV_I(release, HIP_PRINT_ENV, 0,  "Print HIP environment variables.");
    //-- READ HIP_PRINT_ENV env first, since it has impact on later env var reading

    READ_ENV_I(release, HIP_LAUNCH_BLOCKING, CUDA_LAUNCH_BLOCKING, "Make HIP APIs 'host-synchronous', so they block until any kernel launches or data copy commands complete. Alias: CUDA_LAUNCH_BLOCKING." );
    READ_ENV_I(release, HIP_DB, 0,  "Print various debug info.  Bitmask, see hip_hcc.cpp for more information.");
    if ((HIP_DB & DB_API)  && (HIP_TRACE_API == 0)) {
        // Set HIP_TRACE_API before we read it, so it is printed correctly.
        HIP_TRACE_API = 1;
    }


    READ_ENV_I(release, HIP_TRACE_API, 0,  "Trace each HIP API call.  Print function name and return code to stderr as program executes.");
    READ_ENV_I(release, HIP_STAGING_SIZE, 0, "Size of each staging buffer (in KB)" );
    READ_ENV_I(release, HIP_STAGING_BUFFERS, 0, "Number of staging buffers to use in each direction. 0=use hsa_memory_copy.");
    READ_ENV_I(release, HIP_PININPLACE, 0, "For unpinned transfers, pin the memory in-place in chunks before doing the copy. Under development.");
    READ_ENV_I(release, HIP_STREAM_SIGNALS, 0, "Number of signals to allocate when new stream is created (signal pool will grow on demand)");
    READ_ENV_I(release, HIP_VISIBLE_DEVICES, CUDA_VISIBLE_DEVICES, "Only devices whose index is present in the secquence are visible to HIP applications and they are enumerated in the order of secquence" );

    READ_ENV_I(release, HIP_DISABLE_HW_KERNEL_DEP, 0, "Disable HW dependencies before kernel commands  - instead wait for dependency on host. -1 means ignore these dependencies. (debug mode)");
    READ_ENV_I(release, HIP_DISABLE_HW_COPY_DEP, 0, "Disable HW dependencies before copy commands  - instead wait for dependency on host. -1 means ifnore these dependencies (debug mode)");

    if (HIP_DB && !COMPILE_HIP_DB) {
        fprintf (stderr, "warning: env var HIP_DB=0x%x but COMPILE_HIP_DB=0.  (perhaps enable COMPILE_HIP_DB in src code before compiling?)", HIP_DB);
    }

    if (HIP_TRACE_API && !COMPILE_HIP_TRACE_API) {
        fprintf (stderr, "warning: env var HIP_TRACE_API=0x%x but COMPILE_HIP_TRACE_API=0.  (perhaps enable COMPILE_HIP_DB in src code before compiling?)", HIP_DB);
    }


    /*
     * Build a table of valid compute devices.
     */
    auto accs = hc::accelerator::get_all();
    int deviceCnt = 0;
    for (int i=0; i<accs.size(); i++) {
        if (! accs[i].get_is_emulated()) {
            deviceCnt++;
        }
    };

    // Make sure the hip visible devices are within the deviceCnt range
    for (int i = 0; i < g_hip_visible_devices.size(); i++) {
        if(g_hip_visible_devices[i] >= deviceCnt){
            // Make sure any DeviceID after invalid DeviceID will be erased.
            g_hip_visible_devices.resize(i);
            break;
        }
    }

    g_devices = new ihipDevice_t[deviceCnt];
    g_deviceCnt = 0;
    for (int i=0; i<accs.size(); i++) {
        // check if the device id is included in the HIP_VISIBLE_DEVICES env variable
        if (! accs[i].get_is_emulated()) {
            //if (std::find(g_hip_visible_devices.begin(), g_hip_visible_devices.end(), (i-1)) == g_hip_visible_devices.end() && g_visible_device)
            if (std::find(g_hip_visible_devices.begin(), g_hip_visible_devices.end(), (i-1)) == g_hip_visible_devices.end() && g_visible_device)
            {
                //If device is not in visible devices list, ignore
                continue;
            }
            g_devices[g_deviceCnt].init(g_deviceCnt, accs[i]);
            g_deviceCnt++;
        }
    }

    // If HIP_VISIBLE_DEVICES is not set, make sure all devices are initialized
    if(!g_visible_device) {
        assert(deviceCnt == g_deviceCnt);
    }


    hsa_status_t err = hsa_iterate_agents(findCpuAgent, &g_cpu_agent);
    if (err != HSA_STATUS_INFO_BREAK) {
        // didn't find a CPU.
        throw ihipException(hipErrorRuntimeOther);
    }


    tprintf(DB_SYNC, "pid=%u %-30s\n", getpid(), "<ihipInit>");
}



// TODO - data-up to data-down:
// Called just before a kernel is launched from hipLaunchKernel.
// Allows runtime to track some information about the stream.
hipStream_t ihipPreLaunchKernel(hipStream_t stream, hc::accelerator_view **av)
{
	std::call_once(hip_initialized, ihipInit);
    stream = ihipSyncAndResolveStream(stream);

    stream->preKernelCommand();

    *av = &stream->_av;

    return (stream);
}


//---
//Called after kernel finishes execution.
void ihipPostLaunchKernel(hipStream_t stream, hc::completion_future &kernelFuture)
{
    stream->postKernelCommand(kernelFuture);
    if (HIP_LAUNCH_BLOCKING) {
        tprintf(DB_SYNC, " stream:%p LAUNCH_BLOCKING for kernel completion\n", stream);
    }
}


//
//=================================================================================================
// HIP API Implementation
//
// Implementor notes:
// _ All functions should call ihipInit as first action:
//    std::call_once(hip_initialized, ihipInit);
//
// - ALl functions should use ihipLogStatus to return error code (not return error directly).
//=================================================================================================
//
//---







//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
// Error Handling
//---
/**
 * @returns return code from last HIP called from the active host thread.
 */
hipError_t hipGetLastError()
{
    HIP_INIT_API();

    // Return last error, but then reset the state:
    hipError_t e = ihipLogStatus(tls_lastHipError);
    tls_lastHipError = hipSuccess;
    return e;
}


//---
hipError_t hipPeakAtLastError()
{
    HIP_INIT_API();

   
    // peak at last error, but don't reset it. 
    return ihipLogStatus(tls_lastHipError);
}


const char *ihipErrorString(hipError_t hip_error)
{
    switch (hip_error) {
        case hipSuccess                     : return "hipSuccess";
        case hipErrorMemoryAllocation       : return "hipErrorMemoryAllocation";
        case hipErrorMemoryFree             : return "hipErrorMemoryFree";
        case hipErrorUnknownSymbol          : return "hipErrorUnknownSymbol";
        case hipErrorOutOfResources         : return "hipErrorOutOfResources";
        case hipErrorInvalidValue           : return "hipErrorInvalidValue";
        case hipErrorInvalidResourceHandle  : return "hipErrorInvalidResourceHandle";
        case hipErrorInvalidDevice          : return "hipErrorInvalidDevice";
        case hipErrorInvalidMemcpyDirection : return "hipErrorInvalidMemcpyDirection";
        case hipErrorNoDevice               : return "hipErrorNoDevice";
        case hipErrorNotReady               : return "hipErrorNotReady";
        case hipErrorRuntimeMemory          : return "hipErrorRuntimeMemory";
        case hipErrorRuntimeOther           : return "hipErrorRuntimeOther";
        case hipErrorUnknown                : return "hipErrorUnknown";
        case hipErrorTbd                    : return "hipErrorTbd";
        default                             : return "hipErrorUnknown";
    };
};



//---
const char *hipGetErrorName(hipError_t hip_error)
{
    HIP_INIT_API(hip_error);

    return ihipErrorString(hip_error);
}


/**
 * @warning : hipGetErrorString returns string from hipGetErrorName
 */

//---
const char *hipGetErrorString(hipError_t hip_error)
{
    std::call_once(hip_initialized, ihipInit);

    // TODO - return a message explaining the error.
    // TODO - This should be set up to return the same string reported in the the doxygen comments, somehow.
    return hipGetErrorName(hip_error);
}


//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
// Stream
//

//---
hipError_t hipStreamCreateWithFlags(hipStream_t *stream, unsigned int flags)
{
    std::call_once(hip_initialized, ihipInit);

    ihipDevice_t *device = ihipGetTlsDefaultDevice();
    hc::accelerator acc = device->_acc;

    // TODO - se try-catch loop to detect memory exception?
    //
    //
    //Note this is an execute_in_order queue, so all kernels submitted will atuomatically wait for prev to complete:
    //This matches CUDA stream behavior:

    auto istream = new ihipStream_t(device->_device_index, acc.create_view(), device->_stream_id, flags);
    device->_streams.push_back(istream);
    *stream = istream;
    tprintf(DB_SYNC, "hipStreamCreate, stream=%p\n", *stream);

    return ihipLogStatus(hipSuccess);
}


//---
/**
 * @bug This function conservatively waits for all work in the specified stream to complete.
 */
hipError_t hipStreamWaitEvent(hipStream_t stream, hipEvent_t event, unsigned int flags)
{

    std::call_once(hip_initialized, ihipInit);

    hipError_t e = hipSuccess;

    {
        // TODO-hcc Convert to use create_blocking_marker(...) functionality.
        // Currently we have a super-conservative version of this - block on host, and drain the queue.
        // This should create a barrier packet in the target queue.
        stream->wait();
        e = hipSuccess;
    }

    return ihipLogStatus(e);
};


//---
hipError_t hipStreamSynchronize(hipStream_t stream)
{
    std::call_once(hip_initialized, ihipInit);

    hipError_t e = hipSuccess;

    if (stream == NULL) {
        ihipDevice_t *device = ihipGetTlsDefaultDevice();
        device->syncDefaultStream(true/*waitOnSelf*/);
    } else {
        stream->wait();
        e = hipSuccess;
    }


    return ihipLogStatus(e);
};


//---
/**
 * @return #hipSuccess, #hipErrorInvalidResourceHandle
 */
hipError_t hipStreamDestroy(hipStream_t stream)
{
    std::call_once(hip_initialized, ihipInit);

    hipError_t e = hipSuccess;

    //--- Drain the stream:
    if (stream == NULL) {
        ihipDevice_t *device = ihipGetTlsDefaultDevice();
        device->syncDefaultStream(true/*waitOnSelf*/);
    } else {
        stream->wait();
        e = hipSuccess;
    }

    ihipDevice_t *device = stream->getDevice();

    if (device) {
        device->_streams.remove(stream);
        delete stream;
    } else {
        e = hipErrorInvalidResourceHandle;
    }

    return ihipLogStatus(e);
}


//---
hipError_t hipStreamGetFlags(hipStream_t stream, unsigned int *flags)
{
    std::call_once(hip_initialized, ihipInit);

    if (flags == NULL) {
        return ihipLogStatus(hipErrorInvalidValue);
    } else if (stream == NULL) {
        return ihipLogStatus(hipErrorInvalidResourceHandle);
    } else {
        *flags = stream->_flags;
        return ihipLogStatus(hipSuccess);
    }
}



//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
// Events
//---
/**
 * @warning : flags must be 0.
 */
hipError_t hipEventCreateWithFlags(hipEvent_t* event, unsigned flags)
{
    // TODO - support hipEventDefault, hipEventBlockingSync, hipEventDisableTiming
    std::call_once(hip_initialized, ihipInit);

    hipError_t e = hipSuccess;

    if (flags == 0) {
        ihipEvent_t *eh = event->_handle = new ihipEvent_t();

        eh->_state  = hipEventStatusCreated;
        eh->_stream = NULL;
        eh->_flags  = flags;
        eh->_timestamp  = 0;
        eh->_copy_seq_id  = 0;
    } else {
        e = hipErrorInvalidValue;
    }


    return ihipLogStatus(e);
}


//---
hipError_t hipEventRecord(hipEvent_t event, hipStream_t stream)
{
    std::call_once(hip_initialized, ihipInit);

    ihipEvent_t *eh = event._handle;
    if (eh && eh->_state != hipEventStatusUnitialized)   {
        eh->_stream = stream;

        if (stream == NULL) {
            // If stream == NULL, wait on all queues.
            // This matches behavior described in CUDA 7 RT APIs, which say that "This function uses standard default stream semantics".
            // TODO-HCC fix this - is CUDA this conservative or still uses device timestamps?
            // TODO-HCC can we use barrier or event marker to implement better solution?
            ihipDevice_t *device = ihipGetTlsDefaultDevice();
            device->syncDefaultStream(true);

            eh->_timestamp = hc::get_system_ticks();
            eh->_state = hipEventStatusRecorded;
            return ihipLogStatus(hipSuccess);
        } else {
            eh->_state  = hipEventStatusRecording;
            // Clear timestamps
            eh->_timestamp = 0;
            eh->_marker = stream->_av.create_marker();
            eh->_copy_seq_id = stream->lastCopySeqId();

            return ihipLogStatus(hipSuccess);
        }
    } else {
        return ihipLogStatus(hipErrorInvalidResourceHandle);
    }
}


//---
hipError_t hipEventDestroy(hipEvent_t event)
{
    std::call_once(hip_initialized, ihipInit);

    event._handle->_state  = hipEventStatusUnitialized;

    delete event._handle;
    event._handle = NULL;

    // TODO - examine return additional error codes
    return ihipLogStatus(hipSuccess);
}


//---
hipError_t hipEventSynchronize(hipEvent_t event)
{
    std::call_once(hip_initialized, ihipInit);

    ihipEvent_t *eh = event._handle;

    if (eh) {
        if (eh->_state == hipEventStatusUnitialized) {
            return ihipLogStatus(hipErrorInvalidResourceHandle);
        } else if (eh->_state == hipEventStatusCreated ) {
            // Created but not actually recorded on any device:
            return ihipLogStatus(hipSuccess);
        } else if (eh->_stream == NULL) {
            ihipDevice_t *device = ihipGetTlsDefaultDevice();
            device->syncDefaultStream(true);
            return ihipLogStatus(hipSuccess);
        } else {
#if __hcc_workweek__ >= 16033
            eh->_marker.wait((eh->_flags & hipEventBlockingSync) ? hc::hcWaitModeBlocked : hc::hcWaitModeActive);
#else
            eh->_marker.wait();
#endif
            eh->_stream->reclaimSignals_ts(eh->_copy_seq_id);

            return ihipLogStatus(hipSuccess);
        }
    } else {
        return ihipLogStatus(hipErrorInvalidResourceHandle);
    }
}


void ihipSetTs(hipEvent_t e)
{
    ihipEvent_t *eh = e._handle;
    if (eh->_state == hipEventStatusRecorded) {
        // already recorded, done:
        return;
    } else {
        // TODO - use completion-future functions to obtain ticks and timestamps:
        hsa_signal_t *sig  = static_cast<hsa_signal_t*> (eh->_marker.get_native_handle());
        if (sig) {
            if (hsa_signal_load_acquire(*sig) == 0) {
                eh->_timestamp = eh->_marker.get_end_tick();
                eh->_state = hipEventStatusRecorded;
            }
        }
    }
}


//---
hipError_t hipEventElapsedTime(float *ms, hipEvent_t start, hipEvent_t stop)
{
    std::call_once(hip_initialized, ihipInit);

    ihipEvent_t *start_eh = start._handle;
    ihipEvent_t *stop_eh = stop._handle;

    ihipSetTs(start);
    ihipSetTs(stop);

    hipError_t status = hipSuccess;
    *ms = 0.0f;

    if (start_eh && stop_eh) {
        if ((start_eh->_state == hipEventStatusRecorded) && (stop_eh->_state == hipEventStatusRecorded)) {
            // Common case, we have good information for both events.

            int64_t tickDiff = (stop_eh->_timestamp - start_eh->_timestamp);

            // TODO-move this to a variable saved with each agent.
            uint64_t freqHz;
            hsa_system_get_info(HSA_SYSTEM_INFO_TIMESTAMP_FREQUENCY, &freqHz);
            if (freqHz) {
                *ms = ((double)(tickDiff) /  (double)(freqHz)) * 1000.0f;
                status = hipSuccess;
            } else {
                * ms = 0.0f;
                status = hipErrorInvalidValue;
            }


        } else if ((start_eh->_state == hipEventStatusRecording) ||
                   (stop_eh->_state  == hipEventStatusRecording)) {
            status = hipErrorNotReady;
        } else if ((start_eh->_state == hipEventStatusUnitialized) ||
                   (stop_eh->_state  == hipEventStatusUnitialized)) {
            status = hipErrorInvalidResourceHandle;
        }
    }

    return ihipLogStatus(status);
}


//---
hipError_t hipEventQuery(hipEvent_t event)
{
    std::call_once(hip_initialized, ihipInit);

    ihipEvent_t *eh = event._handle;

    // TODO-stream - need to read state of signal here:  The event may have become ready after recording..
    // TODO-HCC - use get_hsa_signal here.

    if (eh->_state == hipEventStatusRecording) {
        return ihipLogStatus(hipErrorNotReady);
    } else {
        return ihipLogStatus(hipSuccess);
    }
}



//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
// Memory
//
//
//

//---
/**
 * @return #hipSuccess, #hipErrorInvalidValue, #hipErrorInvalidDevice
 */
hipError_t hipPointerGetAttributes(hipPointerAttribute_t *attributes, void* ptr)
{
    std::call_once(hip_initialized, ihipInit);

    hipError_t e = hipSuccess;

    hc::accelerator acc;
    hc::AmPointerInfo amPointerInfo(NULL, NULL, 0, acc, 0, 0);
    am_status_t status = hc::am_memtracker_getinfo(&amPointerInfo, ptr);
    if (status == AM_SUCCESS) {

        attributes->memoryType    = amPointerInfo._isInDeviceMem ? hipMemoryTypeDevice: hipMemoryTypeHost;
        attributes->hostPointer   = amPointerInfo._hostPointer;
        attributes->devicePointer = amPointerInfo._devicePointer;
        attributes->isManaged     = 0;
        if(attributes->memoryType == hipMemoryTypeHost){
            attributes->hostPointer = ptr;
        }
        if(attributes->memoryType == hipMemoryTypeDevice){
            attributes->devicePointer = ptr;
        }
        attributes->allocationFlags = amPointerInfo._appAllocationFlags;
        attributes->device          = amPointerInfo._appId;

        if (attributes->device < 0) {
            e = hipErrorInvalidDevice;
        }


    } else {
        attributes->memoryType    = hipMemoryTypeDevice;
        attributes->hostPointer   = 0;
        attributes->devicePointer = 0;
        attributes->device        = -1;
        attributes->isManaged     = 0;
        attributes->allocationFlags = 0;

        e = hipErrorUnknown; // TODO - should be hipErrorInvalidValue ?
    }

    return ihipLogStatus(e);
}


/**
 * @returns #hipSuccess,
 * @returns #hipErrorInvalidValue if flags are not 0
 * @returns #hipErrorMemoryAllocation if hostPointer is not a tracked allocation.
 */
hipError_t hipHostGetDevicePointer(void **devicePointer, void *hostPointer, unsigned flags)
{
    std::call_once(hip_initialized, ihipInit);

    hipError_t e = hipSuccess;

    // Flags must be 0:
    if (flags != 0) {
        e = hipErrorInvalidValue;
    } else {
        hc::accelerator acc;
        hc::AmPointerInfo amPointerInfo(NULL, NULL, 0, acc, 0, 0);
        am_status_t status = hc::am_memtracker_getinfo(&amPointerInfo, hostPointer);
        if (status == AM_SUCCESS) {
            *devicePointer = amPointerInfo._devicePointer;
        } else {
            e = hipErrorMemoryAllocation;
            *devicePointer = NULL;
        }
    }

    return ihipLogStatus(e);
}



// kernel for launching memcpy operations:
template <typename T>
hc::completion_future
ihipMemcpyKernel(hipStream_t stream, T * c, const T * a, size_t sizeBytes)
{
    int wg = std::min((unsigned)8, stream->getDevice()->_compute_units);
    const int threads_per_wg = 256;

    int threads = wg * threads_per_wg;
    if (threads > sizeBytes) {
        threads = ((sizeBytes + threads_per_wg - 1) / threads_per_wg) * threads_per_wg;
    }


    hc::extent<1> ext(threads);
    auto ext_tile = ext.tile(threads_per_wg);

    hc::completion_future cf =
    hc::parallel_for_each(
            stream->_av,
            ext_tile,
            [=] (hc::tiled_index<1> idx)
            __attribute__((hc))
    {
        int offset = amp_get_global_id(0);
        // TODO-HCC - change to hc_get_local_size()
        int stride = amp_get_local_size(0) * hc_get_num_groups(0) ;

        for (int i=offset; i<sizeBytes; i+=stride) {
            c[i] = a[i];
        }
    });

    return cf;
}


// kernel for launching memset operations:
template <typename T>
hc::completion_future
ihipMemsetKernel(hipStream_t stream, T * ptr, T val, size_t sizeBytes)
{
    int wg = std::min((unsigned)8, stream->getDevice()->_compute_units);
    const int threads_per_wg = 256;

    int threads = wg * threads_per_wg;
    if (threads > sizeBytes) {
        threads = ((sizeBytes + threads_per_wg - 1) / threads_per_wg) * threads_per_wg;
    }


    hc::extent<1> ext(threads);
    auto ext_tile = ext.tile(threads_per_wg);

    hc::completion_future cf =
    hc::parallel_for_each(
            stream->_av,
            ext_tile,
            [=] (hc::tiled_index<1> idx)
            __attribute__((hc))
    {
        int offset = amp_get_global_id(0);
        // TODO-HCC - change to hc_get_local_size()
        int stride = amp_get_local_size(0) * hc_get_num_groups(0) ;

        for (int i=offset; i<sizeBytes; i+=stride) {
            ptr[i] = val;
        }
    });

    return cf;

}

//---
/**
 * @returns #hipSuccess #hipErrorMemoryAllocation
 */
hipError_t hipMalloc(void** ptr, size_t sizeBytes)
{
    HIP_INIT_API(ptr, sizeBytes);

    hipError_t  hip_status = hipSuccess;

	auto device = ihipGetTlsDefaultDevice();

    if (device) {
        const unsigned am_flags = 0;
        *ptr = hc::am_alloc(sizeBytes, device->_acc, am_flags);

        if (sizeBytes && (*ptr == NULL)) {
            hip_status = hipErrorMemoryAllocation;
        } else {
            hc::am_memtracker_update(*ptr, device->_device_index, 0);
        }
    } else {
        hip_status = hipErrorMemoryAllocation;
    }

    return ihipLogStatus(hip_status);
}


hipError_t hipMallocHost(void** ptr, size_t sizeBytes)
{
    HIP_INIT_API(ptr, sizeBytes);

    hipError_t  hip_status = hipSuccess;

    const unsigned am_flags = amHostPinned;
	auto device = ihipGetTlsDefaultDevice();

    if (device) {
        *ptr = hc::am_alloc(sizeBytes, device->_acc, am_flags);
        if (sizeBytes && (*ptr == NULL)) {
            hip_status = hipErrorMemoryAllocation;
        } else {
            hc::am_memtracker_update(*ptr, device->_device_index, 0);
        }

        tprintf (DB_MEM, "  %s: pinned ptr=%p\n", __func__, *ptr);
    }

    return ihipLogStatus(hip_status);
}


hipError_t hipHostMalloc(void** ptr, size_t sizeBytes, unsigned int flags)
{
    HIP_INIT_API(ptr, sizeBytes, flags);

    hipError_t hip_status = hipSuccess;

    auto device = ihipGetTlsDefaultDevice();

    if(device){
        if(flags == hipHostMallocDefault){
            *ptr = hc::am_alloc(sizeBytes, device->_acc, amHostPinned);
            if(sizeBytes && (*ptr == NULL)){
                hip_status = hipErrorMemoryAllocation;
            }else{
                hc::am_memtracker_update(*ptr, device->_device_index, 0);
            }
            tprintf(DB_MEM, " %s: pinned ptr=%p\n", __func__, *ptr);
        } else if(flags & hipHostMallocMapped){
            *ptr = hc::am_alloc(sizeBytes, device->_acc, amHostPinned);
            if(sizeBytes && (*ptr == NULL)){
                hip_status = hipErrorMemoryAllocation;
            }else{
                hc::am_memtracker_update(*ptr, device->_device_index, flags);
            }
            tprintf(DB_MEM, " %s: pinned ptr=%p\n", __func__, *ptr);
        }
    }
    return ihipLogStatus(hip_status);
}


// TODO - remove me, this is deprecated.
hipError_t hipHostAlloc(void** ptr, size_t sizeBytes, unsigned int flags)
{
    return hipHostMalloc(ptr, sizeBytes, flags);
};


hipError_t hipHostGetFlags(unsigned int* flagsPtr, void* hostPtr)
{
    HIP_INIT_API(flagsPtr, hostPtr);

	hipError_t hip_status = hipSuccess;

	hc::accelerator acc;
	hc::AmPointerInfo amPointerInfo(NULL, NULL, 0, acc, 0, 0);
	am_status_t status = hc::am_memtracker_getinfo(&amPointerInfo, hostPtr);
	if(status == AM_SUCCESS){
		*flagsPtr = amPointerInfo._appAllocationFlags;
		if(*flagsPtr == 0){
			hip_status = hipErrorInvalidValue;
		}
		else{
			hip_status = hipSuccess;
		}
		tprintf(DB_MEM, " %s: host ptr=%p\n", __func__, hostPtr);
	}else{
		hip_status = hipErrorInvalidValue;
	}
	return ihipLogStatus(hip_status);
}

hipError_t hipHostRegister(void *hostPtr, size_t sizeBytes, unsigned int flags)
{
    HIP_INIT_API(hostPtr, sizeBytes, flags);

	hipError_t hip_status = hipSuccess;

	auto device = ihipGetTlsDefaultDevice();
	void* srcPtr;
	if(hostPtr == NULL){
		return ihipLogStatus(hipErrorInvalidValue);
	}
	if(device){
	if(flags == hipHostRegisterDefault){
		hsa_status_t hsa_status = hsa_amd_memory_lock(hostPtr, sizeBytes, &device->_hsa_agent, 1, &srcPtr);
		if(hsa_status == HSA_STATUS_SUCCESS){
			hip_status = hipSuccess;	
		}else{
			hip_status = hipErrorMemoryAllocation;
		}
	}
	else if (flags | hipHostRegisterMapped){
		hsa_status_t hsa_status = hsa_amd_memory_lock(hostPtr, sizeBytes, &device->_hsa_agent, 1, &srcPtr);
		//TODO: Added feature for actual host pointer being tracked
		if(hsa_status != HSA_STATUS_SUCCESS){
			hip_status = hipErrorMemoryAllocation;
		}
	}
	}
	return ihipLogStatus(hip_status);
}

hipError_t hipHostUnregister(void *hostPtr)
{
    HIP_INIT_API(hostPtr);

	hipError_t hip_status = hipSuccess;
	if(hostPtr == NULL){
		hip_status = hipErrorInvalidValue;
	}else{
	hsa_status_t hsa_status = hsa_amd_memory_unlock(hostPtr);
	if(hsa_status != HSA_STATUS_SUCCESS){
		hip_status = hipErrorInvalidValue;
// TODO: Add a different return error. This is not true
	}
	}
	return ihipLogStatus(hip_status);
}


//---
hipError_t hipMemcpyToSymbol(const char* symbolName, const void *src, size_t count, size_t offset, hipMemcpyKind kind)
{
    HIP_INIT_API(symbolName, src, count, offset, kind);

#ifdef USE_MEMCPYTOSYMBOL
	if(kind != hipMemcpyHostToDevice)
	{
		return ihipLogStatus(hipErrorInvalidValue);
	}
	auto device = ihipGetTlsDefaultDevice();

    //hsa_signal_t depSignal;
    //int depSignalCnt = device._default_stream->preCopyCommand(NULL, &depSignal, ihipCommandCopyH2D);
    assert(0);  // Need to properly synchronize the copy - do something with depSignal if != NULL.

	device->_acc.memcpy_symbol(symbolName, (void*) src,count, offset);
#endif
    return ihipLogStatus(hipSuccess);
}


// Resolve hipMemcpyDefault to a known type.
hipMemcpyKind ihipStream_t::resolveMemcpyDirection(bool srcInDeviceMem, bool dstInDeviceMem)
{
    hipMemcpyKind kind = hipMemcpyDefault;

    if (!srcInDeviceMem && !dstInDeviceMem) {
        kind = hipMemcpyHostToHost;
    } else if (!srcInDeviceMem && dstInDeviceMem) {
        kind = hipMemcpyHostToDevice;
    } else if (srcInDeviceMem && !dstInDeviceMem) {
        kind = hipMemcpyDeviceToHost;
    } else if (srcInDeviceMem &&  dstInDeviceMem) {
        kind = hipMemcpyDeviceToDevice;
    }

    assert (kind != hipMemcpyDefault);

    return kind;
}


// Setup the copyCommandType and the copy agents (for hsa_amd_memory_async_copy)
void ihipStream_t::setCopyAgents(hipMemcpyKind kind, ihipCommand_t *commandType, hsa_agent_t *srcAgent, hsa_agent_t *dstAgent)
{
    ihipDevice_t *device = this->getDevice();
    hsa_agent_t deviceAgent = device->_hsa_agent;

    switch (kind) {
        case hipMemcpyHostToHost     : *commandType = ihipCommandCopyH2H; *srcAgent=g_cpu_agent; *dstAgent=g_cpu_agent; break;
        case hipMemcpyHostToDevice   : *commandType = ihipCommandCopyH2D; *srcAgent=g_cpu_agent; *dstAgent=deviceAgent; break;
        case hipMemcpyDeviceToHost   : *commandType = ihipCommandCopyD2H; *srcAgent=deviceAgent; *dstAgent=g_cpu_agent; break;
        case hipMemcpyDeviceToDevice : *commandType = ihipCommandCopyD2D; *srcAgent=deviceAgent; *dstAgent=deviceAgent; break;
        default: throw ihipException(hipErrorInvalidMemcpyDirection);
    };
}


void ihipStream_t::copySync(void* dst, const void* src, size_t sizeBytes, hipMemcpyKind kind)
{
    ihipDevice_t *device = this->getDevice();

    if (device == NULL) {
        throw ihipException(hipErrorInvalidDevice);
    }

    hc::accelerator acc;
    hc::AmPointerInfo dstPtrInfo(NULL, NULL, 0, acc, 0, 0);
    hc::AmPointerInfo srcPtrInfo(NULL, NULL, 0, acc, 0, 0);

    bool dstTracked = (hc::am_memtracker_getinfo(&dstPtrInfo, dst) == AM_SUCCESS);
    bool srcTracked = (hc::am_memtracker_getinfo(&srcPtrInfo, src) == AM_SUCCESS);


    // Resolve default to a specific Kind so we know which algorithm to use:
    if (kind == hipMemcpyDefault) {
        bool srcInDeviceMem = (srcTracked && srcPtrInfo._isInDeviceMem);
        bool dstInDeviceMem = (dstTracked && dstPtrInfo._isInDeviceMem);
        kind = resolveMemcpyDirection(srcInDeviceMem, dstInDeviceMem);
    };

    hsa_signal_t depSignal;

    if ((kind == hipMemcpyHostToDevice) && (!srcTracked)) {
        int depSignalCnt = preCopyCommand(NULL, &depSignal, ihipCommandCopyH2D);
        if (HIP_STAGING_BUFFERS) {
            tprintf(DB_COPY1, "D2H && !dstTracked: staged copy H2D dst=%p src=%p sz=%zu\n", dst, src, sizeBytes);

            if (HIP_PININPLACE) {
                device->_staging_buffer[0]->CopyHostToDevicePinInPlace(dst, src, sizeBytes, depSignalCnt ? &depSignal : NULL);
            } else  {
                device->_staging_buffer[0]->CopyHostToDevice(dst, src, sizeBytes, depSignalCnt ? &depSignal : NULL);
            }

            // The copy waits for inputs and then completes before returning so can reset queue to empty:
            this->wait(true);
        } else {
            // TODO - remove, slow path.
            tprintf(DB_COPY1, "H2D && ! srcTracked: am_copy dst=%p src=%p sz=%zu\n", dst, src, sizeBytes);
#if USE_AV_COPY
            _av.copy(src,dst,sizeBytes);
#else
            hc::am_copy(dst, src, sizeBytes);
#endif
        }
    } else if ((kind == hipMemcpyDeviceToHost) && (!dstTracked)) {
        int depSignalCnt = preCopyCommand(NULL, &depSignal, ihipCommandCopyD2H);
        if (HIP_STAGING_BUFFERS) {
            tprintf(DB_COPY1, "D2H && !dstTracked: staged copy D2H dst=%p src=%p sz=%zu\n", dst, src, sizeBytes);
            //printf ("staged-copy- read dep signals\n");
            device->_staging_buffer[1]->CopyDeviceToHost(dst, src, sizeBytes, depSignalCnt ? &depSignal : NULL);

            // The copy waits for inputs and then completes before returning so can reset queue to empty:
            this->wait(true);

        } else {
            // TODO - remove, slow path.
            tprintf(DB_COPY1, "D2H && !dstTracked: am_copy dst=%p src=%p sz=%zu\n", dst, src, sizeBytes);
#if USE_AV_COPY
            _av.copy(src, dst, sizeBytes);
#else
            hc::am_copy(dst, src, sizeBytes);
#endif
        }
    } else if (kind == hipMemcpyHostToHost)  { 
        int depSignalCnt = preCopyCommand(NULL, &depSignal, ihipCommandCopyH2H);

        if (depSignalCnt) {
            // host waits before doing host memory copy.
            hsa_signal_wait_acquire(depSignal, HSA_SIGNAL_CONDITION_LT, 1, UINT64_MAX, HSA_WAIT_STATE_ACTIVE);
        }
        tprintf(DB_COPY1, "H2H memcpy dst=%p src=%p sz=%zu\n", dst, src, sizeBytes);
        memcpy(dst, src, sizeBytes);

    } else {
        // If not special case - these can all be handled by the hsa async copy:
        ihipCommand_t commandType;
        hsa_agent_t srcAgent, dstAgent;
        setCopyAgents(kind, &commandType, &srcAgent, &dstAgent);

        int depSignalCnt = preCopyCommand(NULL, &depSignal, commandType);

        // Get a completion signal:
        ihipSignal_t *ihipSignal = allocSignal();
        hsa_signal_t copyCompleteSignal = ihipSignal->_hsa_signal;

        hsa_signal_store_relaxed(copyCompleteSignal, 1);

        tprintf(DB_COPY1, "HSA Async_copy dst=%p src=%p sz=%zu\n", dst, src, sizeBytes);

        hsa_status_t hsa_status = hsa_amd_memory_async_copy(dst, dstAgent, src, srcAgent, sizeBytes, depSignalCnt, depSignalCnt ? &depSignal:0x0, copyCompleteSignal);

        // This is sync copy, so let's wait for copy right here:
        if (hsa_status == HSA_STATUS_SUCCESS) {
            waitCopy(ihipSignal); // wait for copy, and return to pool.
        } else {
            throw ihipException(hipErrorInvalidValue);
        }
    }
}




void ihipStream_t::copyAsync(void* dst, const void* src, size_t sizeBytes, hipMemcpyKind kind)
{
    ihipDevice_t *device = this->getDevice();

    if (device == NULL) {
        throw ihipException(hipErrorInvalidDevice);
    }

    if (kind == hipMemcpyHostToHost) {
        tprintf (DB_COPY2, "Asyc: H2H with memcpy");

        // TODO - consider if we want to perhaps use the GPU SDMA engines anyway, to avoid the host-side sync here and keep everything flowing on the GPU.
        /* As this is a CPU op, we need to wait until all
        the commands in current stream are finished.
        */
        this->wait();

        memcpy(dst, src, sizeBytes);

    } else {
        bool trueAsync = true;

        hc::accelerator acc;
        hc::AmPointerInfo dstPtrInfo(NULL, NULL, 0, acc, 0, 0);
        hc::AmPointerInfo srcPtrInfo(NULL, NULL, 0, acc, 0, 0);
        bool dstTracked = (hc::am_memtracker_getinfo(&dstPtrInfo, dst) == AM_SUCCESS);
        bool srcTracked = (hc::am_memtracker_getinfo(&srcPtrInfo, src) == AM_SUCCESS);


        // "tracked" really indicates if the pointer's virtual address is available in the GPU address space.  
        // If both pointers are not tracked, we need to fall back to a sync copy.
        if (!dstTracked || !srcTracked) {
            trueAsync = false;
        }

        if (kind == hipMemcpyDefault) {
            bool srcInDeviceMem = (srcTracked && srcPtrInfo._isInDeviceMem);
            bool dstInDeviceMem = (dstTracked && dstPtrInfo._isInDeviceMem);
            kind = resolveMemcpyDirection(srcInDeviceMem, dstInDeviceMem);
        }



        ihipSignal_t *ihip_signal = allocSignal();
        hsa_signal_store_relaxed(ihip_signal->_hsa_signal, 1);


        if(trueAsync == true){

            ihipCommand_t commandType;
            hsa_agent_t srcAgent, dstAgent;
            setCopyAgents(kind, &commandType, &srcAgent, &dstAgent);

            hsa_signal_t depSignal;
            int depSignalCnt = preCopyCommand(ihip_signal, &depSignal, commandType);

            tprintf (DB_SYNC, " copy-async, waitFor=%lu completion=#%lu(%lu)\n", depSignalCnt? depSignal.handle:0x0, ihip_signal->_sig_id, ihip_signal->_hsa_signal.handle);

            hsa_status_t hsa_status = hsa_amd_memory_async_copy(dst, dstAgent, src, srcAgent, sizeBytes, depSignalCnt, depSignalCnt ? &depSignal:0x0, ihip_signal->_hsa_signal);


            if (hsa_status == HSA_STATUS_SUCCESS) {
                if (HIP_LAUNCH_BLOCKING) {
                    tprintf(DB_SYNC, "LAUNCH_BLOCKING for completion of hipMemcpyAsync(%zu)\n", sizeBytes);
                    this->wait();
                }
            } else {
                // This path can be hit if src or dst point to unpinned host memory.
                // TODO-stream - does async-copy fall back to sync if input pointers are not pinned?
                throw ihipException(hipErrorInvalidValue);
            }
        } else {
            copySync(dst, src, sizeBytes, kind);
        }
    }
}


//---
hipError_t hipMemcpy(void* dst, const void* src, size_t sizeBytes, hipMemcpyKind kind)
{
    HIP_INIT_API(dst, src, sizeBytes, kind);

    hipStream_t stream = ihipSyncAndResolveStream(hipStreamNull);

    hc::completion_future marker;

    hipError_t e = hipSuccess;

    try {
        stream->copySync(dst, src, sizeBytes, kind);
    }
    catch (ihipException ex) {
        e = ex._code;
    }


    if (HIP_LAUNCH_BLOCKING) {
        tprintf(DB_SYNC, "LAUNCH_BLOCKING for completion of hipMemcpy\n");
        stream->wait();
    }

    return ihipLogStatus(e);
}


/**
 * @result #hipSuccess, #hipErrorInvalidDevice, #hipErrorInvalidMemcpyDirection, 
 * @result #hipErrorInvalidValue : If dst==NULL or src==NULL, or other bad argument.
 * @warning on HCC hipMemcpyAsync does not support overlapped H2D and D2H copies.
 * @warning on HCC hipMemcpyAsync requires that any host pointers are pinned (ie via the hipMallocHost call).
 */
//---
hipError_t hipMemcpyAsync(void* dst, const void* src, size_t sizeBytes, hipMemcpyKind kind, hipStream_t stream)
{
    HIP_INIT_API(dst, src, sizeBytes, kind, stream);

    hipError_t e = hipSuccess;

    stream = ihipSyncAndResolveStream(stream);


    if ((dst == NULL) || (src == NULL)) {
        e= hipErrorInvalidValue;
    } else if (stream) {
        try {
            stream->copyAsync(dst, src, sizeBytes, kind);
        }
        catch (ihipException ex) {
            e = ex._code;
        }
    } else {
        e = hipErrorInvalidValue;
    }

    return ihipLogStatus(e);
}


// TODO-sync: function is async unless target is pinned host memory - then these are fully sync.
/** @return #hipErrorInvalidValue
 */
hipError_t hipMemsetAsync(void* dst, int  value, size_t sizeBytes, hipStream_t stream )
{
    HIP_INIT_API(dst, value, sizeBytes, stream);

    hipError_t e = hipSuccess;

    stream =  ihipSyncAndResolveStream(stream);
    stream->preKernelCommand();

    if (stream) {

        hc::completion_future cf ;

        if ((sizeBytes & 0x3) == 0) {
            // use a faster word-per-workitem copy:
            try {
                value = value & 0xff;
                unsigned value32 = (value << 24) | (value << 16) | (value << 8) | (value) ;
                cf = ihipMemsetKernel<unsigned> (stream, static_cast<unsigned*> (dst), value32, sizeBytes/sizeof(unsigned));
            }
            catch (std::exception &ex) {
                e = hipErrorInvalidValue;
            }
        } else {
            // use a slow byte-per-workitem copy:
            try {
                cf = ihipMemsetKernel<char> (stream, static_cast<char*> (dst), value, sizeBytes);
            }
            catch (std::exception &ex) {
                e = hipErrorInvalidValue;
            }
        }

        stream->postKernelCommand(cf);


        if (HIP_LAUNCH_BLOCKING) {
            tprintf (DB_SYNC, "'%s' LAUNCH_BLOCKING wait for completion [stream:%p].\n", __func__, (void*)stream);
            cf.wait();
            tprintf (DB_SYNC, "'%s' LAUNCH_BLOCKING completed [stream:%p].\n", __func__, (void*)stream);
        }
    } else {
        e = hipErrorInvalidValue;
    }


    return ihipLogStatus(e);
};


hipError_t hipMemset(void* dst, int  value, size_t sizeBytes )
{
    HIP_INIT_API(dst, value, sizeBytes);

    // TODO - call an ihip memset so HIP_TRACE is correct.
    return hipMemsetAsync(dst, value, sizeBytes, hipStreamNull);
}


/*
 * @returns #hipSuccess, #hipErrorInvalidDevice, #hipErrorInvalidValue (if free != NULL due to bug)S
 * @warning On HCC, the free memory only accounts for memory allocated by this process and may be optimistic.
 */
hipError_t hipMemGetInfo  (size_t *free, size_t *total)
{
    HIP_INIT_API(free, total);

    hipError_t e = hipSuccess;

    ihipDevice_t * hipDevice = ihipGetTlsDefaultDevice();
    if (hipDevice) {
        if (total) {
            *total = hipDevice->_props.totalGlobalMem;
        }

        if (free) {
            // TODO - replace with kernel-level for reporting free memory:
            size_t deviceMemSize, hostMemSize, userMemSize;
            hc::am_memtracker_sizeinfo(hipDevice->_acc, &deviceMemSize, &hostMemSize, &userMemSize);
            printf ("deviceMemSize=%zu\n", deviceMemSize);
        
            *free =  hipDevice->_props.totalGlobalMem - deviceMemSize;
        }

    } else {
        e = hipErrorInvalidDevice;
    }

    return ihipLogStatus(e);
}


//---
hipError_t hipFree(void* ptr)
{
    HIP_INIT_API(ptr);

    hipError_t hipStatus = hipErrorInvalidDevicePointer;

   // Synchronize to ensure all work has finished.
    ihipGetTlsDefaultDevice()->waitAllStreams(); // ignores non-blocking streams, this waits for all activity to finish.

    if (ptr) {
        hc::accelerator acc;
        hc::AmPointerInfo amPointerInfo(NULL, NULL, 0, acc, 0, 0);
        am_status_t status = hc::am_memtracker_getinfo(&amPointerInfo, ptr);
        if(status == AM_SUCCESS){
            if(amPointerInfo._hostPointer == NULL){
                hc::am_free(ptr);
                hipStatus = hipSuccess;
            }
        }
    }

    return ihipLogStatus(hipStatus);
}


hipError_t hipHostFree(void* ptr)
{
    HIP_INIT_API(ptr);

    // TODO - ensure this pointer was created by hipMallocHost and not hipMalloc
    std::call_once(hip_initialized, ihipInit);

    hipError_t hipStatus = hipErrorInvalidDevicePointer;
    if (ptr) {
        hc::accelerator acc;
        hc::AmPointerInfo amPointerInfo(NULL, NULL, 0, acc, 0, 0);
        am_status_t status = hc::am_memtracker_getinfo(&amPointerInfo, ptr);
        if(status == AM_SUCCESS){
            if(amPointerInfo._hostPointer == ptr){
                hc::am_free(ptr);
                hipStatus = hipSuccess;
            }
        }
    }

    return ihipLogStatus(hipStatus);
};


// TODO - deprecated function.
hipError_t hipFreeHost(void* ptr)
{
    return hipHostFree(ptr);
}



/**
 * @warning HCC returns 0 in *canAccessPeer ; Need to update this function when RT supports P2P
 */
//---
hipError_t hipDeviceCanAccessPeer ( int* canAccessPeer, int  device, int  peerDevice )
{
    HIP_INIT_API(canAccessPeer, device, peerDevice);

    *canAccessPeer = false;
    return ihipLogStatus(hipSuccess);
}


/**
 * @warning Need to update this function when RT supports P2P
 */
//---
hipError_t  hipDeviceDisablePeerAccess ( int  peerDevice )
{
    HIP_INIT_API(peerDevice);

    // TODO-p2p
    return ihipLogStatus(hipSuccess);
};


/**
 * @warning Need to update this function when RT supports P2P
 */
//---
hipError_t  hipDeviceEnablePeerAccess ( int  peerDevice, unsigned int  flags )
{
    std::call_once(hip_initialized, ihipInit);
    // TODO-p2p
    return ihipLogStatus(hipSuccess);
}


//---
hipError_t hipMemcpyPeer ( void* dst, int  dstDevice, const void* src, int  srcDevice, size_t sizeBytes )
{
    std::call_once(hip_initialized, ihipInit);
    // HCC has a unified memory architecture so device specifiers are not required.
    return hipMemcpy(dst, src, sizeBytes, hipMemcpyDefault);
};


/**
 * @bug This function uses a synchronous copy
 */
//---
hipError_t hipMemcpyPeerAsync ( void* dst, int  dstDevice, const void* src, int  srcDevice, size_t sizeBytes, hipStream_t stream )
{
    std::call_once(hip_initialized, ihipInit);
    // HCC has a unified memory architecture so device specifiers are not required.
    return hipMemcpyAsync(dst, src, sizeBytes, hipMemcpyDefault, stream);
};


/**
 * @return #hipSuccess
 */
//---
hipError_t hipDriverGetVersion(int *driverVersion)
{
    HIP_INIT_API(driverVersion);

    if (driverVersion) {
        *driverVersion = 4;
    }

    return ihipLogStatus(hipSuccess);
}



//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
// HCC-specific accessor functions:

/**
 * @return #hipSuccess, #hipErrorInvalidDevice
 */
//---
hipError_t hipHccGetAccelerator(int deviceId, hc::accelerator *acc)
{
    std::call_once(hip_initialized, ihipInit);

    ihipDevice_t *d = ihipGetDevice(deviceId);
    hipError_t err;
    if (d == NULL) {
        err =  hipErrorInvalidDevice;
    } else {
        *acc = d->_acc;
        err = hipSuccess;
    }
    return ihipLogStatus(err);
}


/**
 * @return #hipSuccess
 */
//---
hipError_t hipHccGetAcceleratorView(hipStream_t stream, hc::accelerator_view **av)
{
    std::call_once(hip_initialized, ihipInit);

    if (stream == hipStreamNull ) {
        ihipDevice_t *device = ihipGetTlsDefaultDevice();
        stream = device->_default_stream;
    }

    *av = &(stream->_av);

    hipError_t err = hipSuccess;
    return ihipLogStatus(err);
}

// TODO - review signal / error reporting code.
// TODO - describe naming convention. ihip _.  No accessors.  No early returns from functions. Set status to success at top, only set error codes in implementation.  No tabs.
//        Caps convention _ or camelCase
//        if { }
// TODO - describe MT strategy
//
//// TODO - add identifier numbers for streams and devices to help with debugging.

#if ONE_OBJECT_FILE
#include "staging_buffer.cpp"
//#include "hip_device.cpp"
#endif