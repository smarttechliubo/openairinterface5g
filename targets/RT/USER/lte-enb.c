/*******************************************************************************
    OpenAirInterface
    Copyright(c) 1999 - 2014 Eurecom

    OpenAirInterface is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.


    OpenAirInterface is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with OpenAirInterface.The full GNU General Public License is
    included in this distribution in the file called "COPYING". If not,
    see <http://www.gnu.org/licenses/>.

   Contact Information
   OpenAirInterface Admin: openair_admin@eurecom.fr
   OpenAirInterface Tech : openair_tech@eurecom.fr
   OpenAirInterface Dev  : openair4g-devel@lists.eurecom.fr

   Address      : Eurecom, Campus SophiaTech, 450 Route des Chappes, CS 50193 - 06904 Biot Sophia Antipolis cedex, FRANCE

*******************************************************************************/

/*! \file lte-enb.c
 * \brief Top-level threads for eNodeB
 * \author R. Knopp, F. Kaltenberger, Navid Nikaein
 * \date 2012
 * \version 0.1
 * \company Eurecom
 * \email: knopp@eurecom.fr,florian.kaltenberger@eurecom.fr, navid.nikaein@eurecom.fr
 * \note
 * \warning
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sched.h>
#include <linux/sched.h>
#include <signal.h>
#include <execinfo.h>
#include <getopt.h>
#include <sys/sysinfo.h>
#include "rt_wrapper.h"

#undef MALLOC //there are two conflicting definitions, so we better make sure we don't use it at all

#include "assertions.h"
#include "msc.h"

#include "PHY/types.h"

#include "PHY/defs.h"
#undef MALLOC //there are two conflicting definitions, so we better make sure we don't use it at all
//#undef FRAME_LENGTH_COMPLEX_SAMPLES //there are two conflicting definitions, so we better make sure we don't use it at all

#include "../../ARCH/COMMON/common_lib.h"

//#undef FRAME_LENGTH_COMPLEX_SAMPLES //there are two conflicting definitions, so we better make sure we don't use it at all

#include "PHY/extern.h"
#include "SCHED/extern.h"
#include "LAYER2/MAC/extern.h"

#include "../../SIMU/USER/init_lte.h"

#include "LAYER2/MAC/defs.h"
#include "LAYER2/MAC/extern.h"
#include "LAYER2/MAC/proto.h"
#include "RRC/LITE/extern.h"
#include "PHY_INTERFACE/extern.h"

#ifdef SMBV
#include "PHY/TOOLS/smbv.h"
unsigned short config_frames[4] = {2,9,11,13};
#endif
#include "UTIL/LOG/log_extern.h"
#include "UTIL/OTG/otg_tx.h"
#include "UTIL/OTG/otg_externs.h"
#include "UTIL/MATH/oml.h"
#include "UTIL/LOG/vcd_signal_dumper.h"
#include "UTIL/OPT/opt.h"
#include "enb_config.h"
//#include "PHY/TOOLS/time_meas.h"

#ifndef OPENAIR2
#include "UTIL/OTG/otg_extern.h"
#endif

#if defined(ENABLE_ITTI)
# if defined(ENABLE_USE_MME)
#   include "s1ap_eNB.h"
#ifdef PDCP_USE_NETLINK
#   include "SIMULATION/ETH_TRANSPORT/proto.h"
#endif
# endif
#endif





//#define DEBUG_THREADS 1

//#define USRP_DEBUG 1
struct timing_info_t {
  //unsigned int frame, hw_slot, last_slot, next_slot;
  RTIME time_min, time_max, time_avg, time_last, time_now;
  //unsigned int mbox0, mbox1, mbox2, mbox_target;
  unsigned int n_samples;
} timing_info;


extern openair0_device openair0;

#if defined(ENABLE_ITTI)
extern volatile int             start_eNB;
extern volatile int             start_UE;
#endif
extern volatile int                    oai_exit;

extern openair0_config_t openair0_cfg[MAX_CARDS];

extern pthread_cond_t sync_cond;
extern pthread_mutex_t sync_mutex;
extern int sync_var;

//pthread_t                       main_eNB_thread;

time_stats_t softmodem_stats_mt; // main thread
time_stats_t softmodem_stats_hw; //  hw acquisition
time_stats_t softmodem_stats_tx_sf; // total tx time
time_stats_t softmodem_stats_rx_sf; // total rx time
int32_t **rxdata;
int32_t **txdata;

static int                      time_offset[4] = {0,0,0,0};

/* mutex, cond and variable to serialize phy proc TX calls
 * (this mechanism may be relaxed in the future for better
 * performances)
 */
static struct {
  pthread_mutex_t  mutex_phy_proc_tx;
  pthread_cond_t   cond_phy_proc_tx;
  volatile uint8_t phy_proc_CC_id;
} sync_phy_proc;


void exit_fun(const char* s);

void init_eNB(eNB_func_t node_function);
void stop_eNB(void);

void do_OFDM_mod_rt(int subframe,PHY_VARS_eNB *phy_vars_eNB)
{

  unsigned int aa,slot_offset, slot_offset_F;
  int dummy_tx_b[7680*4] __attribute__((aligned(32)));
  int i, tx_offset;
  int slot_sizeF = (phy_vars_eNB->frame_parms.ofdm_symbol_size)*
                   ((phy_vars_eNB->frame_parms.Ncp==1) ? 6 : 7);
  int len;

  slot_offset_F = (subframe<<1)*slot_sizeF;

  slot_offset = subframe*phy_vars_eNB->frame_parms.samples_per_tti;

  if ((subframe_select(&phy_vars_eNB->frame_parms,subframe)==SF_DL)||
      ((subframe_select(&phy_vars_eNB->frame_parms,subframe)==SF_S))) {
    //    LOG_D(HW,"Frame %d: Generating slot %d\n",frame,next_slot);


    for (aa=0; aa<phy_vars_eNB->frame_parms.nb_antennas_tx; aa++) {
      if (phy_vars_eNB->frame_parms.Ncp == EXTENDED) {
        PHY_ofdm_mod(&phy_vars_eNB->common_vars.txdataF[0][aa][slot_offset_F],
                     dummy_tx_b,
                     phy_vars_eNB->frame_parms.ofdm_symbol_size,
                     6,
                     phy_vars_eNB->frame_parms.nb_prefix_samples,
                     CYCLIC_PREFIX);
        PHY_ofdm_mod(&phy_vars_eNB->common_vars.txdataF[0][aa][slot_offset_F+slot_sizeF],
                     dummy_tx_b+(phy_vars_eNB->frame_parms.samples_per_tti>>1),
                     phy_vars_eNB->frame_parms.ofdm_symbol_size,
                     6,
                     phy_vars_eNB->frame_parms.nb_prefix_samples,
                     CYCLIC_PREFIX);
      } else {
        normal_prefix_mod(&phy_vars_eNB->common_vars.txdataF[0][aa][slot_offset_F],
                          dummy_tx_b,
                          7,
                          &(phy_vars_eNB->frame_parms));
	// if S-subframe generate first slot only
	if (subframe_select(&phy_vars_eNB->frame_parms,subframe) == SF_DL)
	  normal_prefix_mod(&phy_vars_eNB->common_vars.txdataF[0][aa][slot_offset_F+slot_sizeF],
			    dummy_tx_b+(phy_vars_eNB->frame_parms.samples_per_tti>>1),
			    7,
			    &(phy_vars_eNB->frame_parms));
      }

      // if S-subframe generate first slot only
      if (subframe_select(&phy_vars_eNB->frame_parms,subframe) == SF_S)
	len = phy_vars_eNB->frame_parms.samples_per_tti>>1;
      else
	len = phy_vars_eNB->frame_parms.samples_per_tti;
      /*
      for (i=0;i<len;i+=4) {
	dummy_tx_b[i] = 0x100;
	dummy_tx_b[i+1] = 0x01000000;
	dummy_tx_b[i+2] = 0xff00;
	dummy_tx_b[i+3] = 0xff000000;
	}*/
      for (i=0; i<len; i++) {
        tx_offset = (int)slot_offset+time_offset[aa]+i;

	
        if (tx_offset<0)
          tx_offset += LTE_NUMBER_OF_SUBFRAMES_PER_FRAME*phy_vars_eNB->frame_parms.samples_per_tti;

        if (tx_offset>=(LTE_NUMBER_OF_SUBFRAMES_PER_FRAME*phy_vars_eNB->frame_parms.samples_per_tti))
          tx_offset -= LTE_NUMBER_OF_SUBFRAMES_PER_FRAME*phy_vars_eNB->frame_parms.samples_per_tti;

	((short*)&phy_vars_eNB->common_vars.txdata[0][aa][tx_offset])[0] = ((short*)dummy_tx_b)[2*i]<<openair0_cfg[0].iq_txshift;
	
	((short*)&phy_vars_eNB->common_vars.txdata[0][aa][tx_offset])[1] = ((short*)dummy_tx_b)[2*i+1]<<openair0_cfg[0].iq_txshift;
     }
     // if S-subframe switch to RX in second subframe
     if (subframe_select(&phy_vars_eNB->frame_parms,subframe) == SF_S) {
       for (i=0; i<len; i++) {
	 phy_vars_eNB->common_vars.txdata[0][aa][tx_offset++] = 0x00010001;
       }
     }

     if ((((phy_vars_eNB->frame_parms.tdd_config==0) ||
	  (phy_vars_eNB->frame_parms.tdd_config==1) ||
	  (phy_vars_eNB->frame_parms.tdd_config==2) ||
	  (phy_vars_eNB->frame_parms.tdd_config==6)) && 
	  (subframe==0)) || (subframe==5)) {
       // turn on tx switch N_TA_offset before
       //LOG_D(HW,"subframe %d, time to switch to tx (N_TA_offset %d, slot_offset %d) \n",subframe,phy_vars_eNB->N_TA_offset,slot_offset);
       for (i=0; i<phy_vars_eNB->N_TA_offset; i++) {
	 tx_offset = (int)slot_offset+time_offset[aa]+i-phy_vars_eNB->N_TA_offset/2;
	 if (tx_offset<0)
	   tx_offset += LTE_NUMBER_OF_SUBFRAMES_PER_FRAME*phy_vars_eNB->frame_parms.samples_per_tti;
	 
	 if (tx_offset>=(LTE_NUMBER_OF_SUBFRAMES_PER_FRAME*phy_vars_eNB->frame_parms.samples_per_tti))
	   tx_offset -= LTE_NUMBER_OF_SUBFRAMES_PER_FRAME*phy_vars_eNB->frame_parms.samples_per_tti;
	 
	 phy_vars_eNB->common_vars.txdata[0][aa][tx_offset] = 0x00000000;
       }
     }
    }
  }
}

/*!
 * \brief The transmit thread of eNB.
 * \ref NUM_ENB_THREADS threads of this type are active at the same time.
 * \param param is a \ref eNB_proc_t structure which contains the info what to process.
 * \returns a pointer to an int. The storage is not on the heap and must not be freed.
 */
static void* eNB_thread_tx( void* param )
{
  static int eNB_thread_tx_status;

  eNB_proc_t *proc = (eNB_proc_t*)param;
  FILE  *tx_time_file = NULL;
  char tx_time_name[101];
  void *txp[PHY_vars_eNB_g[0][0]->frame_parms.nb_antennas_tx]; 

  if (opp_enabled == 1) {
    snprintf(tx_time_name, 100,"/tmp/%s_tx_time_thread_sf", "eNB");
    tx_time_file = fopen(tx_time_name,"w");
  }
  // set default return value
  eNB_thread_tx_status = 0;

  MSC_START_USE();

#ifdef LOWLATENCY
  struct sched_attr attr;
  unsigned int flags = 0;
  uint64_t runtime  = 850000 ;  
  uint64_t deadline = 1   *  1000000 ; // each tx thread will finish within 1ms
  uint64_t period   = 1   * 10000000; // each tx thread has a period of 10ms from the starting point

  attr.size = sizeof(attr);
  attr.sched_flags = 0;
  attr.sched_nice = 0;
  attr.sched_priority = 0;

  attr.sched_policy   = SCHED_DEADLINE;
  attr.sched_runtime  = runtime;
  attr.sched_deadline = deadline;
  attr.sched_period   = period; 

  if (sched_setattr(0, &attr, flags) < 0 ) {
    perror("[SCHED] eNB tx thread: sched_setattr failed\n");
    return &eNB_thread_tx_status;
  }

  LOG_I( HW, "[SCHED] eNB TX deadline thread (TID %ld) started on CPU %d\n", gettid(), sched_getcpu() );

#else //LOW_LATENCY
  int policy, s, j;
  struct sched_param sparam;
  char cpu_affinity[1024];
  cpu_set_t cpuset;

  /* Set affinity mask to include CPUs 1 to MAX_CPUS */
  /* CPU 0 is reserved for UHD threads */
  /* CPU 1 is reserved for all TX threads */
  /* Enable CPU Affinity only if number of CPUs >2 */
  CPU_ZERO(&cpuset);

#ifdef CPU_AFFINITY
  if (get_nprocs() > 2)
  {
    for (j = 1; j < get_nprocs(); j++)
        CPU_SET(j, &cpuset);
    s = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (s != 0)
    {
      perror( "pthread_setaffinity_np");
      exit_fun("Error setting processor affinity");
    }
  }
#endif //CPU_AFFINITY

  /* Check the actual affinity mask assigned to the thread */

  s = pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
  if (s != 0)
  {
    perror( "pthread_getaffinity_np");
    exit_fun("Error getting processor affinity ");
  }
  memset(cpu_affinity,0,sizeof(cpu_affinity));
  for (j = 0; j < CPU_SETSIZE; j++)
     if (CPU_ISSET(j, &cpuset))
     {  
        char temp[1024];
        sprintf (temp, " CPU_%d", j);
        strcat(cpu_affinity, temp);
     }

  memset(&sparam, 0 , sizeof (sparam));
  sparam.sched_priority = sched_get_priority_max(SCHED_FIFO)-1;
  policy = SCHED_FIFO ; 
  
  s = pthread_setschedparam(pthread_self(), policy, &sparam);
  if (s != 0)
     {
     perror("pthread_setschedparam : ");
     exit_fun("Error setting thread priority");
     }
  s = pthread_getschedparam(pthread_self(), &policy, &sparam);
  if (s != 0)
   {
     perror("pthread_getschedparam : ");
     exit_fun("Error getting thread priority");

   }

 LOG_I( HW, "[SCHED][eNB] TX thread %d started on CPU %d TID %ld, sched_policy = %s , priority = %d, CPU Affinity=%s \n",sched_getcpu(),gettid(),
                   (policy == SCHED_FIFO)  ? "SCHED_FIFO" :
                   (policy == SCHED_RR)    ? "SCHED_RR" :
                   (policy == SCHED_OTHER) ? "SCHED_OTHER" :
                   "???",
                   sparam.sched_priority, cpu_affinity );


#endif //LOW_LATENCY


  mlockall(MCL_CURRENT | MCL_FUTURE);


  while (!oai_exit) {

    VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME( VCD_SIGNAL_DUMPER_FUNCTIONS_eNB_PROC_TX0, 0 );

    if (pthread_mutex_lock(&proc->mutex_tx) != 0) {
      LOG_E( PHY, "[SCHED][eNB] error locking mutex for eNB TX\n");
      exit_fun("nothing to add");
      break;
    }

    while (proc->instance_cnt_tx < 0) {
      // most of the time the thread is waiting here
      // proc->instance_cnt_tx is -1
      pthread_cond_wait( &proc->cond_tx, &proc->mutex_tx ); // this unlocks mutex_tx while waiting and then locks it again
    }

    if (pthread_mutex_unlock(&proc->mutex_tx) != 0) {
      LOG_E(PHY,"[SCHED][eNB] error unlocking mutex for eNB TX\n");
      exit_fun("nothing to add");
      break;
    }

    VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME( VCD_SIGNAL_DUMPER_FUNCTIONS_eNB_PROC_TX0, 1 );
    start_meas( &softmodem_stats_tx_sf );
  
    if (oai_exit) break;

    if (((PHY_vars_eNB_g[0][proc->CC_id]->frame_parms.frame_type == TDD) &&
         ((subframe_select(&PHY_vars_eNB_g[0][proc->CC_id]->frame_parms,proc->subframe_tx) == SF_DL) ||
          (subframe_select(&PHY_vars_eNB_g[0][proc->CC_id]->frame_parms,proc->subframe_tx) == SF_S))) ||
        (PHY_vars_eNB_g[0][proc->CC_id]->frame_parms.frame_type == FDD)) {
      /* run PHY TX procedures the one after the other for all CCs to avoid race conditions
       * (may be relaxed in the future for performance reasons)
       */
      if (pthread_mutex_lock(&sync_phy_proc.mutex_phy_proc_tx) != 0) {
        LOG_E(PHY, "[SCHED][eNB] error locking PHY proc mutex for eNB TX\n");
        exit_fun("nothing to add");
        break;
      }
      /* wait for our turn or oai_exit */
      while (sync_phy_proc.phy_proc_CC_id != proc->CC_id && !oai_exit) {
        pthread_cond_wait(&sync_phy_proc.cond_phy_proc_tx,
                          &sync_phy_proc.mutex_phy_proc_tx);
      }

      if (pthread_mutex_unlock(&sync_phy_proc.mutex_phy_proc_tx) != 0) {
        LOG_E(PHY, "[SCHED][eNB] error unlocking PHY proc mutex for eNB TX\n");
        exit_fun("nothing to add");
      }


      VCD_SIGNAL_DUMPER_DUMP_VARIABLE_BY_NAME( VCD_SIGNAL_DUMPER_VARIABLES_FRAME_NUMBER_TX_ENB, proc->frame_tx );
      VCD_SIGNAL_DUMPER_DUMP_VARIABLE_BY_NAME( VCD_SIGNAL_DUMPER_VARIABLES_SUBFRAME_NUMBER_TX_ENB, proc->subframe_tx );
      if (oai_exit)
        break;
      if (PHY_vars_eNB_g[0][proc->CC_id]->node_function != NGFI_RRU_IF4) { 
	phy_procedures_eNB_TX(PHY_vars_eNB_g[0][proc->CC_id], 0, no_relay, NULL );
	
	/* we're done, let the next one proceed */
	if (pthread_mutex_lock(&sync_phy_proc.mutex_phy_proc_tx) != 0) {
	  LOG_E(PHY, "[SCHED][eNB] error locking PHY proc mutex for eNB TX proc\n");
	  exit_fun("nothing to add");
	  break;
	}
	sync_phy_proc.phy_proc_CC_id++;
	sync_phy_proc.phy_proc_CC_id %= MAX_NUM_CCs;
	pthread_cond_broadcast(&sync_phy_proc.cond_phy_proc_tx);
	if (pthread_mutex_unlock(&sync_phy_proc.mutex_phy_proc_tx) != 0) {
	  LOG_E(PHY, "[SCHED][eNB] error unlocking PHY proc mutex for eNB TX proc\n");
	  exit_fun("nothing to add");
	  break;
	}
      }
    }

    if (PHY_vars_eNB_g[0][proc->CC_id]->node_function != NGFI_RCC_IF4) {
      VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME(VCD_SIGNAL_DUMPER_FUNCTIONS_PHY_ENB_SFGEN , 1 );
      do_OFDM_mod_rt( proc->subframe_tx, PHY_vars_eNB_g[0][proc->CC_id] );
      VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME(VCD_SIGNAL_DUMPER_FUNCTIONS_PHY_ENB_SFGEN , 0 );
    /*
      short *txdata = (short*)&PHY_vars_eNB_g[0][proc->CC_id]->common_vars.txdata[0][0][proc->subframe_tx*PHY_vars_eNB_g[0][proc->CC_id]->frame_parms.samples_per_tti];
      int i;
      for (i=0;i<PHY_vars_eNB_g[0][proc->CC_id]->frame_parms.samples_per_tti*2;i+=8) {
      txdata[i] = 2047;
      txdata[i+1] = 0;
      txdata[i+2] = 0;
      txdata[i+3] = 2047;
      txdata[i+4] = -2047;
      txdata[i+5] = 0;
      txdata[i+6] = 0;
      txdata[i+7] = -2047;      }
    */


      // Transmit TX buffer based on timestamp from RX
    
      VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME( VCD_SIGNAL_DUMPER_FUNCTIONS_TRX_WRITE, 1 );
      // prepare tx buffer pointers
      int i;
      for (i=0; i<PHY_vars_eNB_g[0][0]->frame_parms.nb_antennas_tx; i++)
	txp[i] = (void*)&PHY_vars_eNB_g[0][0]->common_vars.txdata[0][i][proc->subframe_tx*PHY_vars_eNB_g[0][0]->frame_parms.samples_per_tti];
      // if symb_written < spp ==> error 
      if (proc->frame_tx > 50) {
	openair0.trx_write_func(&openair0,
				(proc->timestamp_tx+openair0_cfg[0].tx_sample_advance),
				txp,
				PHY_vars_eNB_g[0][0]->frame_parms.samples_per_tti,
				PHY_vars_eNB_g[0][0]->frame_parms.nb_antennas_tx,
				1);
      }
      VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME( VCD_SIGNAL_DUMPER_FUNCTIONS_TRX_WRITE, 0 );

      VCD_SIGNAL_DUMPER_DUMP_VARIABLE_BY_NAME( VCD_SIGNAL_DUMPER_VARIABLES_TRX_TST, (proc->timestamp_tx-openair0_cfg[0].tx_sample_advance)&0xffffffff );

    }

    if (pthread_mutex_lock(&proc->mutex_tx) != 0) {
      LOG_E( PHY, "[SCHED][eNB] error locking mutex for eNB TX proc\n");
      exit_fun("nothing to add");
      break;
    }

    proc->instance_cnt_tx--;

    if (pthread_mutex_unlock(&proc->mutex_tx) != 0) {
      LOG_E( PHY, "[SCHED][eNB] error unlocking mutex for eNB TX proc\n");
      exit_fun("nothing to add");
      break;
    }

    stop_meas( &softmodem_stats_tx_sf );
#ifdef LOWLATENCY
    if (opp_enabled){
      if(softmodem_stats_tx_sf.diff_now/(cpuf) > attr.sched_runtime){
	VCD_SIGNAL_DUMPER_DUMP_VARIABLE_BY_NAME( VCD_SIGNAL_DUMPER_VARIABLES_RUNTIME_TX_ENB, (softmodem_stats_tx_sf.diff_now/cpuf - attr.sched_runtime)/1000000.0);
      }
    }
#endif 
    print_meas_now(&softmodem_stats_tx_sf,"eNB_TX_SF",tx_time_file);

  }



  VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME( VCD_SIGNAL_DUMPER_FUNCTIONS_eNB_PROC_TX0, 0 );

#ifdef DEBUG_THREADS
  printf( "Exiting eNB thread TX\n");
#endif

  eNB_thread_tx_status = 0;
  return &eNB_thread_tx_status;
}

#if defined(ENABLE_ITTI)
static void wait_system_ready (char *message, volatile int *start_flag)

  /* Wait for eNB application initialization to be complete (eNB registration to MME) */
{
  static char *indicator[] = {".    ", "..   ", "...  ", ".... ", ".....",
			      " ....", "  ...", "   ..", "    .", "     "
  };
  int i = 0;
  
  while ((!oai_exit) && (*start_flag == 0)) {
    LOG_N(EMU, message, indicator[i]);
    fflush(stdout);
    i = (i + 1) % (sizeof(indicator) / sizeof(indicator[0]));
    usleep(200000);
  }
  
  LOG_D(EMU,"\n");
}

#endif

/*!
 * \brief The receive thread of eNB.
 * \ref NUM_ENB_THREADS threads of this type are active at the same time.
 * \param param is a \ref eNB_proc_t structure which contains the info what to process.
 * \returns a pointer to an int. The storage is not on the heap and must not be freed.
 */
static void* eNB_thread_rx( void* param )
{
  static int eNB_thread_rx_status;

  eNB_proc_t *proc = (eNB_proc_t*)param;
  PHY_VARS_eNB *eNB = PHY_vars_eNB_g[0][proc->CC_id];
  LTE_DL_FRAME_PARMS *fp = &eNB->frame_parms;

  FILE  *rx_time_file = NULL;
  char rx_time_name[101];
  //int i;

  if (opp_enabled == 1){
    snprintf(rx_time_name, 100,"/tmp/%s_rx_time_thread_sf", "eNB");
    rx_time_file = fopen(rx_time_name,"w");
  }
  // set default return value
  eNB_thread_rx_status = 0;

  MSC_START_USE();

#ifdef LOWLATENCY
  struct sched_attr attr;
  unsigned int flags = 0;
  uint64_t runtime  = 870000 ;
  uint64_t deadline = 1   *  1000000;
  uint64_t period   = 1   * 10000000; // each rx thread has a period of 10ms from the starting point
 
  attr.size = sizeof(attr);
  attr.sched_flags = 0;
  attr.sched_nice = 0;
  attr.sched_priority = 0;

  attr.sched_policy = SCHED_DEADLINE;
  attr.sched_runtime  = runtime;
  attr.sched_deadline = deadline;
  attr.sched_period   = period; 

  if (sched_setattr(0, &attr, flags) < 0 ) {
    perror("[SCHED] eNB RX sched_setattr failed\n");
    return &eNB_thread_rx_status;
  }

  LOG_I( HW, "[SCHED] eNB RX deadline thread (TID %ld) started on CPU %d\n", gettid(), sched_getcpu() );
#else // LOW_LATENCY
  int policy, s, j;
  struct sched_param sparam;
  char cpu_affinity[1024];
  cpu_set_t cpuset;

  /* Set affinity mask to include CPUs 1 to MAX_CPUS */
  /* CPU 0 is reserved for UHD */
  /* CPU 1 is reserved for all TX threads */
  /* CPU 2..MAX_CPUS is reserved for all RX threads */
  /* Set CPU Affinity only if number of CPUs >2 */
  CPU_ZERO(&cpuset);
#ifdef CPU_AFFINITY
  if (get_nprocs() >2)
  {
    for (j = 1; j < get_nprocs(); j++)
       CPU_SET(j, &cpuset);
  
    s = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (s != 0)
    {
      perror( "pthread_setaffinity_np");  
      exit_fun (" Error setting processor affinity :");
    }
  }
#endif //CPU_AFFINITY
  /* Check the actual affinity mask assigned to the thread */

  s = pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
  if (s != 0)
  {
     perror ("pthread_getaffinity_np");
     exit_fun (" Error getting processor affinity :");
  }
  memset(cpu_affinity,0, sizeof(cpu_affinity));

  for (j = 0; j < CPU_SETSIZE; j++)
     if (CPU_ISSET(j, &cpuset))
     {  
        char temp[1024];
        sprintf (temp, " CPU_%d", j);
        strcat(cpu_affinity, temp);
     }


  memset(&sparam, 0 , sizeof (sparam)); 
  sparam.sched_priority = sched_get_priority_max(SCHED_FIFO)-1;

  policy = SCHED_FIFO ; 
  s = pthread_setschedparam(pthread_self(), policy, &sparam);
  if (s != 0)
     {
     perror("pthread_setschedparam : ");
     exit_fun("Error setting thread priority");
     }

  memset(&sparam, 0 , sizeof (sparam));

  s = pthread_getschedparam(pthread_self(), &policy, &sparam);
  if (s != 0)
   {
     perror("pthread_getschedparam");
     exit_fun("Error getting thread priority");
   }


  LOG_I( HW, "[SCHED][eNB] RX thread started on CPU %d TID %ld, sched_policy = %s, priority = %d, CPU Affinity = %s\n", sched_getcpu(),gettid(),
	 (policy == SCHED_FIFO)  ? "SCHED_FIFO" :
	 (policy == SCHED_RR)    ? "SCHED_RR" :
	 (policy == SCHED_OTHER) ? "SCHED_OTHER" :
	 "???",
	 sparam.sched_priority, cpu_affinity);
  
  
#endif // LOWLATENCY

 mlockall(MCL_CURRENT | MCL_FUTURE);


 // wait for top-level synchronization and do one acquisition to get timestamp for setting frame/subframe of TX and RX threads
 printf( "waiting for sync (eNB_thread_rx)\n");
 pthread_mutex_lock( &sync_mutex );

 while (sync_var<0)
   pthread_cond_wait( &sync_cond, &sync_mutex );
 
 pthread_mutex_unlock(&sync_mutex);
 
 printf( "got sync (eNB_thread)\n" );
 
#if defined(ENABLE_ITTI)
  wait_system_ready ("Waiting for eNB application to be ready %s\r", &start_eNB);
#endif 

 // This is a forever while loop, it loops over subframes which are scheduled by incoming samples from HW devices
 while (!oai_exit) {
   
   
   VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME( VCD_SIGNAL_DUMPER_FUNCTIONS_eNB_PROC_RX0, 0 );
   VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME( VCD_SIGNAL_DUMPER_FUNCTIONS_PHY_PROCEDURES_ENB_RX_COMMON, 0 );
   start_meas( &softmodem_stats_rx_sf );
   
   if (oai_exit) break;
   
   if ((((fp->frame_type == TDD )&&(subframe_select(fp,proc->subframe_rx)==SF_UL)) ||
	(fp->frame_type == FDD))) {
     VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME( VCD_SIGNAL_DUMPER_FUNCTIONS_PHY_PROCEDURES_ENB_RX_COMMON, 1 );
     // this spawns the prach and TX threads inside and updates the frame and subframe counters
     phy_procedures_eNB_common_RX(eNB, 0);
     VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME( VCD_SIGNAL_DUMPER_FUNCTIONS_PHY_PROCEDURES_ENB_RX_COMMON, 0 );
     if (eNB->node_function != NGFI_RRU_IF4) {
       VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME( VCD_SIGNAL_DUMPER_FUNCTIONS_PHY_PROCEDURES_ENB_RX_UESPEC, 1 );
       // this is the ue-specific processing for the subframe and can be multi-threaded later
       phy_procedures_eNB_uespec_RX(eNB, 0, no_relay );
       VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME( VCD_SIGNAL_DUMPER_FUNCTIONS_PHY_PROCEDURES_ENB_RX_UESPEC, 0 );
     }
   }
   
   if ((subframe_select(fp,proc->subframe_rx) == SF_S)) {
     phy_procedures_eNB_S_RX(eNB, 0, no_relay );
   }
   
   stop_meas( &softmodem_stats_rx_sf );
#ifdef LOWLATENCY
   if (opp_enabled){
     if(softmodem_stats_rx_sf.diff_now/(cpuf) > attr.sched_runtime){
       VCD_SIGNAL_DUMPER_DUMP_VARIABLE_BY_NAME( VCD_SIGNAL_DUMPER_VARIABLES_RUNTIME_RX_ENB, (softmodem_stats_rx_sf.diff_now/cpuf - attr.sched_runtime)/1000000.0);
     }
   }
#endif // LOWLATENCY  
   print_meas_now(&softmodem_stats_rx_sf,"eNB_RX_SF", rx_time_file);
   VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME( VCD_SIGNAL_DUMPER_FUNCTIONS_eNB_PROC_RX0, 0 );
   

 }

  //stop_meas( &softmodem_stats_rx_sf[proc->thread_index] );
  VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME( VCD_SIGNAL_DUMPER_FUNCTIONS_eNB_PROC_RX0, 0 );


#ifdef DEBUG_THREADS
  printf( "Exiting eNB thread RX\n");
#endif

  eNB_thread_rx_status = 0;
  return &eNB_thread_rx_status;
}



/*!
 * \brief The prach receive thread of eNB.
 * \param param is a \ref eNB_proc_t structure which contains the info what to process.
 * \returns a pointer to an int. The storage is not on the heap and must not be freed.
 */
static void* eNB_thread_prach( void* param )
{
  static int eNB_thread_prach_status;

  eNB_proc_t *proc = (eNB_proc_t*)param;
  PHY_VARS_eNB *eNB= PHY_vars_eNB_g[0][proc->CC_id];
  // set default return value
  eNB_thread_prach_status = 0;

  MSC_START_USE();

    
#ifdef LOWLATENCY
  struct sched_attr attr;
  unsigned int flags = 0;
  uint64_t runtime  = 870000 ;
  uint64_t deadline = 1   *  1000000;
  uint64_t period   = 1   * 10000000; // each prach thread has a period of 10ms from the starting point
 
  attr.size = sizeof(attr);
  attr.sched_flags = 0;
  attr.sched_nice = 0;
  attr.sched_priority = 0;

  attr.sched_policy = SCHED_DEADLINE;
  attr.sched_runtime  = runtime;
  attr.sched_deadline = deadline;
  attr.sched_period   = period; 

  if (sched_setattr(0, &attr, flags) < 0 ) {
    perror("[SCHED] eNB PRACH sched_setattr failed\n");
    return &eNB_thread_prach_status;
  }

  LOG_I( HW, "[SCHED] eNB PRACH deadline thread (TID %ld) started on CPU %d\n", 0, gettid(), sched_getcpu() );
#else // LOW_LATENCY
  int policy, s, j;
  struct sched_param sparam;
  char cpu_affinity[1024];
  cpu_set_t cpuset;

  /* Set affinity mask to include CPUs 1 to MAX_CPUS */
  /* CPU 0 is reserved for UHD */
  /* CPU 1 is reserved for all TX threads */
  /* CPU 2..MAX_CPUS is reserved for all RX threads */
  /* Set CPU Affinity only if number of CPUs >2 */
  CPU_ZERO(&cpuset);
#ifdef CPU_AFFINITY
  if (get_nprocs() >2)
  {
    for (j = 1; j < get_nprocs(); j++)
       CPU_SET(j, &cpuset);
  
    s = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (s != 0)
    {
      perror( "pthread_setaffinity_np");  
      exit_fun (" Error setting processor affinity :");
    }
  }
#endif //CPU_AFFINITY
  /* Check the actual affinity mask assigned to the thread */

  s = pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
  if (s != 0)
  {
     perror ("pthread_getaffinity_np");
     exit_fun (" Error getting processor affinity :");
  }
  memset(cpu_affinity,0, sizeof(cpu_affinity));

  for (j = 0; j < CPU_SETSIZE; j++)
     if (CPU_ISSET(j, &cpuset))
     {  
        char temp[1024];
        sprintf (temp, " CPU_%d", j);
        strcat(cpu_affinity, temp);
     }


  memset(&sparam, 0 , sizeof (sparam)); 
  sparam.sched_priority = sched_get_priority_max(SCHED_FIFO)-1;

  policy = SCHED_FIFO ; 
  s = pthread_setschedparam(pthread_self(), policy, &sparam);
  if (s != 0)
     {
     perror("pthread_setschedparam : ");
     exit_fun("Error setting thread priority");
     }

  memset(&sparam, 0 , sizeof (sparam));

  s = pthread_getschedparam(pthread_self(), &policy, &sparam);
  if (s != 0)
   {
     perror("pthread_getschedparam");
     exit_fun("Error getting thread priority");
   }


  LOG_I( HW, "[SCHED][eNB] RX thread %d started on CPU %d TID %ld, sched_policy = %s, priority = %d, CPU Affinity = %s\n", sched_getcpu(),gettid(),
	 (policy == SCHED_FIFO)  ? "SCHED_FIFO" :
	 (policy == SCHED_RR)    ? "SCHED_RR" :
	 (policy == SCHED_OTHER) ? "SCHED_OTHER" :
	 "???",
	 sparam.sched_priority, cpu_affinity);
  
  
#endif // LOWLATENCY

 mlockall(MCL_CURRENT | MCL_FUTURE);


 while (!oai_exit) {
 
   
   if (oai_exit) break;
        
   if (pthread_mutex_lock(&proc->mutex_prach) != 0) {
     LOG_E( PHY, "[SCHED][eNB] error locking mutex for eNB PRACH\n");
     exit_fun( "error locking mutex" );
     break;
   }

   while (proc->instance_cnt_prach < 0) {
     // most of the time the thread is waiting here
     // proc->instance_cnt_tx is -1
     pthread_cond_wait( &proc->cond_prach, &proc->mutex_prach ); // this unlocks mutex_tx while waiting and then locks it again
   }

   if (pthread_mutex_unlock(&proc->mutex_prach) != 0) {
     LOG_E( PHY, "[SCHED][eNB] error unlocking mutex for eNB PRACH\n");
     exit_fun( "error unlocking mutex" );
     break;
   }
   
   VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME(VCD_SIGNAL_DUMPER_FUNCTIONS_PHY_ENB_PRACH_RX,1);
   prach_procedures(eNB,0);
   VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME(VCD_SIGNAL_DUMPER_FUNCTIONS_PHY_ENB_PRACH_RX,0);
    
   if (pthread_mutex_lock(&proc->mutex_prach) != 0) {
     LOG_E( PHY, "[SCHED][eNB] error locking mutex for eNB PRACH proc %d\n");
     exit_fun( "error locking mutex" );
     break;
   }
   
   proc->instance_cnt_prach--;
   
   if (pthread_mutex_unlock(&proc->mutex_prach) != 0) {
     LOG_E( PHY, "[SCHED][eNB] error unlocking mutex for eNB RX proc %d\n");
     exit_fun( "error unlocking mutex" );
     break;
   }
 }

#ifdef DEBUG_THREADS
  printf( "Exiting eNB thread PRACH\n");
#endif

  eNB_thread_prach_status = 0;
  return &eNB_thread_prach_status;
}




void init_eNB_proc(void)
{
  int i;
  int CC_id;
  PHY_VARS_eNB *eNB;
  eNB_proc_t *proc;

  for (CC_id=0; CC_id<MAX_NUM_CCs; CC_id++) {
    eNB = PHY_vars_eNB_g[0][CC_id];

    
    proc = &eNB->proc;

#ifndef LOWLATENCY 
    /*  
	pthread_attr_init( &attr_eNB_proc_tx[CC_id][i] );
	if (pthread_attr_setstacksize( &attr_eNB_proc_tx[CC_id][i], 64 *PTHREAD_STACK_MIN ) != 0)
        perror("[ENB_PROC_TX] setting thread stack size failed\n");
	
	pthread_attr_init( &attr_eNB_proc_rx[CC_id][i] );
	if (pthread_attr_setstacksize( &attr_eNB_proc_rx[CC_id][i], 64 * PTHREAD_STACK_MIN ) != 0)
        perror("[ENB_PROC_RX] setting thread stack size failed\n");
    */
    // set the kernel scheduling policy and priority
    proc->sched_param_tx.sched_priority = sched_get_priority_max(SCHED_FIFO)-1; //OPENAIR_THREAD_PRIORITY;
    pthread_attr_setschedparam  (&proc->attr_tx, &proc->sched_param_tx);
    pthread_attr_setschedpolicy (&proc->attr_tx, SCHED_FIFO);
    
    proc->sched_param_rx.sched_priority = sched_get_priority_max(SCHED_FIFO)-1; //OPENAIR_THREAD_PRIORITY;
    pthread_attr_setschedparam  (&proc->attr_rx, &proc->sched_param_rx);
    pthread_attr_setschedpolicy (&proc->attr_rx, SCHED_FIFO);
    
    proc->sched_param_prach.sched_priority = sched_get_priority_max(SCHED_FIFO)-1; //OPENAIR_THREAD_PRIORITY;
    pthread_attr_setschedparam  (&proc->attr_prach, &proc->sched_param_prach);
    pthread_attr_setschedpolicy (&proc->attr_prach, SCHED_FIFO);
    
    printf("Setting OS scheduler to SCHED_FIFO for eNB [cc%d][thread%d] \n",CC_id, i);
#endif
    proc->instance_cnt_tx = -1;
    proc->instance_cnt_prach = -1;
    proc->CC_id = CC_id;
    pthread_mutex_init( &proc->mutex_tx, NULL);
    pthread_cond_init( &proc->cond_tx, NULL);
    pthread_cond_init( &proc->cond_prach, NULL);
#ifndef LOWLATENCY
    pthread_create( &proc->pthread_tx, &proc->attr_tx, eNB_thread_tx, &eNB->proc );
    pthread_create( &proc->pthread_rx, &proc->attr_rx, eNB_thread_rx, &eNB->proc );
    pthread_create( &proc->pthread_prach, &proc->attr_prach, eNB_thread_prach, &eNB->proc );
#else 
    pthread_create( &proc->pthread_tx, NULL, eNB_thread_tx, &eNB->proc );
    pthread_create( &proc->pthread_rx, NULL, eNB_thread_rx, &eNB->proc );
    pthread_create( &proc->pthread_prach, NULL, eNB_thread_prach, &eNB->proc );
#endif
    char name[16];
    snprintf( name, sizeof(name), "TX %d", i );
    pthread_setname_np( proc->pthread_tx, name );
    snprintf( name, sizeof(name), "RX %d", i );
    pthread_setname_np( proc->pthread_rx, name );
  }
  
  
  /* setup PHY proc TX sync mechanism */
  pthread_mutex_init(&sync_phy_proc.mutex_phy_proc_tx, NULL);
  pthread_cond_init(&sync_phy_proc.cond_phy_proc_tx, NULL);
  sync_phy_proc.phy_proc_CC_id = 0;
}


/*!
 * \brief Terminate eNB TX and RX threads.
 */
void kill_eNB_proc(void)
{
  int *status;
  PHY_VARS_eNB *eNB;
  eNB_proc_t *proc;
  for (int CC_id=0; CC_id<MAX_NUM_CCs; CC_id++) {
    eNB=PHY_vars_eNB_g[0][CC_id];
    
    proc = &eNB->proc;
    
#ifdef DEBUG_THREADS
    printf( "Killing TX CC_id %d thread %d\n", CC_id, i );
#endif
    
    proc->instance_cnt_tx = 0; // FIXME data race!
    pthread_cond_signal( &proc->cond_tx );
    pthread_cond_broadcast(&sync_phy_proc.cond_phy_proc_tx);
    
#ifdef DEBUG_THREADS
    printf( "Joining eNB TX CC_id %d thread %d...\n", CC_id, i );
#endif
    int result = pthread_join( proc->pthread_tx, (void**)&status );
    
#ifdef DEBUG_THREADS
    
    if (result != 0) {
      printf( "Error joining thread.\n" );
    } else {
      if (status) {
	printf( "status %d\n", *status );
      } else {
	printf( "The thread was killed. No status available.\n" );
      }
    }
    
#else
    UNUSED(result)
#endif
      
#ifdef DEBUG_THREADS
      printf( "Killing RX CC_id %d thread %d\n", CC_id, i );
#endif
    
#ifdef DEBUG_THREADS
    printf( "Joining eNB RX CC_id %d thread %d...\n", CC_id, i );
#endif
    result = pthread_join( proc->pthread_rx, (void**)&status );
    
#ifdef DEBUG_THREADS
    
    if (result != 0) {
      printf( "Error joining thread.\n" );
    } else {
      if (status) {
	printf( "status %d\n", *status );
      } else {
	printf( "The thread was killed. No status available.\n" );
      }
    }
    
#else
    UNUSED(result);
#endif
            
    pthread_mutex_destroy( &proc->mutex_tx );
    pthread_cond_destroy( &proc->cond_tx );
  }
}


/* this function maps the phy_vars_eNB tx and rx buffers to the available rf chains.
   Each rf chain is is addressed by the card number and the chain on the card. The
   rf_map specifies for each CC, on which rf chain the mapping should start. Multiple
   antennas are mapped to successive RF chains on the same card. */
int setup_eNB_buffers(PHY_VARS_eNB **phy_vars_eNB, openair0_config_t *openair0_cfg, openair0_rf_map rf_map[MAX_NUM_CCs])
{

  int i, CC_id;
#ifndef EXMIMO
  uint16_t N_TA_offset = 0;
#else
  int j;
#endif
  LTE_DL_FRAME_PARMS *frame_parms;


  for (CC_id=0; CC_id<MAX_NUM_CCs; CC_id++) {
    if (phy_vars_eNB[CC_id]) {
      frame_parms = &(phy_vars_eNB[CC_id]->frame_parms);
      printf("setup_eNB_buffers: frame_parms = %p\n",frame_parms);
    } else {
      printf("phy_vars_eNB[%d] not initialized\n", CC_id);
      return(-1);
    }

#ifndef EXMIMO

    if (frame_parms->frame_type == TDD) {
      if (frame_parms->N_RB_DL == 100)
        N_TA_offset = 624;
      else if (frame_parms->N_RB_DL == 50)
        N_TA_offset = 624/2;
      else if (frame_parms->N_RB_DL == 25)
        N_TA_offset = 624/4;
    }

#endif

    // replace RX signal buffers with mmaped HW versions
#ifdef EXMIMO
    openair0_cfg[CC_id].tx_num_channels = 0;
    openair0_cfg[CC_id].rx_num_channels = 0;

    for (i=0; i<frame_parms->nb_antennas_rx; i++) {
      printf("Mapping eNB CC_id %d, rx_ant %d, freq %u on card %d, chain %d\n",CC_id,i,downlink_frequency[CC_id][i]+uplink_frequency_offset[CC_id][i],rf_map[CC_id].card,rf_map[CC_id].chain+i);
      free(phy_vars_eNB[CC_id]->common_vars.rxdata[0][i]);
      phy_vars_eNB[CC_id]->common_vars.rxdata[0][i] = (int32_t*) openair0_exmimo_pci[rf_map[CC_id].card].adc_head[rf_map[CC_id].chain+i];

      if (openair0_cfg[rf_map[CC_id].card].rx_freq[rf_map[CC_id].chain+i]) {
        printf("Error with rf_map! A channel has already been allocated!\n");
        return(-1);
      } else {
        openair0_cfg[rf_map[CC_id].card].rx_freq[rf_map[CC_id].chain+i] = downlink_frequency[CC_id][i]+uplink_frequency_offset[CC_id][i];
        openair0_cfg[rf_map[CC_id].card].rx_gain[rf_map[CC_id].chain+i] = rx_gain[CC_id][i];
        openair0_cfg[rf_map[CC_id].card].rx_num_channels++;
      }

      printf("rxdata[%d] @ %p\n",i,phy_vars_eNB[CC_id]->common_vars.rxdata[0][i]);

      for (j=0; j<16; j++) {
        printf("rxbuffer %d: %x\n",j,phy_vars_eNB[CC_id]->common_vars.rxdata[0][i][j]);
        phy_vars_eNB[CC_id]->common_vars.rxdata[0][i][j] = 16-j;
      }
    }

    for (i=0; i<frame_parms->nb_antennas_tx; i++) {
      printf("Mapping eNB CC_id %d, tx_ant %d, freq %u on card %d, chain %d\n",CC_id,i,downlink_frequency[CC_id][i],rf_map[CC_id].card,rf_map[CC_id].chain+i);
      free(phy_vars_eNB[CC_id]->common_vars.txdata[0][i]);
      phy_vars_eNB[CC_id]->common_vars.txdata[0][i] = (int32_t*) openair0_exmimo_pci[rf_map[CC_id].card].dac_head[rf_map[CC_id].chain+i];

      if (openair0_cfg[rf_map[CC_id].card].tx_freq[rf_map[CC_id].chain+i]) {
        printf("Error with rf_map! A channel has already been allocated!\n");
        return(-1);
      } else {
        openair0_cfg[rf_map[CC_id].card].tx_freq[rf_map[CC_id].chain+i] = downlink_frequency[CC_id][i];
        openair0_cfg[rf_map[CC_id].card].tx_gain[rf_map[CC_id].chain+i] = tx_gain[CC_id][i];
        openair0_cfg[rf_map[CC_id].card].tx_num_channels++;
      }

      printf("txdata[%d] @ %p\n",i,phy_vars_eNB[CC_id]->common_vars.txdata[0][i]);

      for (j=0; j<16; j++) {
        printf("txbuffer %d: %x\n",j,phy_vars_eNB[CC_id]->common_vars.txdata[0][i][j]);
        phy_vars_eNB[CC_id]->common_vars.txdata[0][i][j] = 16-j;
      }
    }

#else // not EXMIMO
    rxdata = (int32_t**)malloc16(frame_parms->nb_antennas_rx*sizeof(int32_t*));
    txdata = (int32_t**)malloc16(frame_parms->nb_antennas_tx*sizeof(int32_t*));

    for (i=0; i<frame_parms->nb_antennas_rx; i++) {
      free(phy_vars_eNB[CC_id]->common_vars.rxdata[0][i]);
      rxdata[i] = (int32_t*)(32 + malloc16(32+openair0_cfg[rf_map[CC_id].card].samples_per_frame*sizeof(int32_t))); // FIXME broken memory allocation
      phy_vars_eNB[CC_id]->common_vars.rxdata[0][i] = rxdata[i]-N_TA_offset; // N_TA offset for TDD         FIXME! N_TA_offset > 16 => access of unallocated memory
      memset(rxdata[i], 0, openair0_cfg[rf_map[CC_id].card].samples_per_frame*sizeof(int32_t));
      printf("rxdata[%d] @ %p (%p) (N_TA_OFFSET %d)\n", i, phy_vars_eNB[CC_id]->common_vars.rxdata[0][i],rxdata[i],N_TA_offset);
      
    }

    for (i=0; i<frame_parms->nb_antennas_tx; i++) {
      free(phy_vars_eNB[CC_id]->common_vars.txdata[0][i]);
      txdata[i] = (int32_t*)(32 + malloc16(32 + openair0_cfg[rf_map[CC_id].card].samples_per_frame*sizeof(int32_t))); // FIXME broken memory allocation
      phy_vars_eNB[CC_id]->common_vars.txdata[0][i] = txdata[i];
      memset(txdata[i],0, openair0_cfg[rf_map[CC_id].card].samples_per_frame*sizeof(int32_t));
      printf("txdata[%d] @ %p\n", i, phy_vars_eNB[CC_id]->common_vars.txdata[0][i]);

    }

#endif
  }

  return(0);
}




void reset_opp_meas(void) {
  int sfn;
  reset_meas(&softmodem_stats_mt);
  reset_meas(&softmodem_stats_hw);
  
  for (sfn=0; sfn < 10; sfn++) {
    reset_meas(&softmodem_stats_tx_sf);
    reset_meas(&softmodem_stats_rx_sf);
  }
}

void print_opp_meas(void) {

  int sfn=0;
  print_meas(&softmodem_stats_mt, "Main ENB Thread", NULL, NULL);
  print_meas(&softmodem_stats_hw, "HW Acquisation", NULL, NULL);
  
  for (sfn=0; sfn < 10; sfn++) {
    print_meas(&softmodem_stats_tx_sf,"[eNB][total_phy_proc_tx]",NULL, NULL);
    print_meas(&softmodem_stats_rx_sf,"[eNB][total_phy_proc_rx]",NULL,NULL);
  }
}


void init_eNB(eNB_func_t node_function) {


  int CC_id;

  for (CC_id=0;CC_id<MAX_NUM_CCs;CC_id++)
    PHY_vars_eNB_g[0][CC_id]->node_function = node_function;

  init_eNB_proc();
  sleep(1);
  LOG_D(HW,"[lte-softmodem.c] eNB threads created\n");
  
  /*  
  printf("Creating main eNB_thread \n");
  error_code = pthread_create( &main_eNB_thread, &attr_dlsch_threads, eNB_thread, NULL );
  
  if (error_code!= 0) {
    LOG_D(HW,"[lte-softmodem.c] Could not allocate eNB_thread, error %d\n",error_code);
  } else {
    LOG_D( HW, "[lte-softmodem.c] Allocate eNB_thread successful\n" );
    pthread_setname_np( main_eNB_thread, "main eNB" );
  }
  */
}


void stop_eNB() {

  /*
#ifdef DEBUG_THREADS
  printf("Joining eNB_thread ...");
#endif
    int *eNB_thread_status_p;
    int result = pthread_join( main_eNB_thread, (void**)&eNB_thread_status_p );

#ifdef DEBUG_THREADS
  
  if (result != 0) {
    printf( "\nError joining main_eNB_thread.\n" );
  } else {
    if (eNB_thread_status_p) {
      printf( "status %d\n", *eNB_thread_status_p );
    } else {
      printf( "The thread was killed. No status available.\n");
    }
  }
  
#else
  UNUSED(result);
#endif // DEBUG_THREADS
  */
  
  printf("Killing eNB processing threads\n");
  kill_eNB_proc();
}
