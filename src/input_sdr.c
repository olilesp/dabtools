/*
This file is part of rtl-dab
trl-dab is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Foobar is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with rtl-dab.  If not, see <http://www.gnu.org/licenses/>.


david may 2012
david.may.muc@googlemail.com

*/

#include "dab.h"
#include "dab_tables.h"
#include "dab_constants.h"
#include "input_sdr.h"
#include "sdr_sync.h"

int sdr_demod(struct demapped_transmission_frame_t *tf, struct sdr_state_t *sdr){
  int i,j;

  //fprintf(stderr, "sdr_demod\n");
  //fprintf(stderr, ".");
  tf->has_fic = 0;

  /* resetting coarse freqshift */
  sdr->coarse_freq_shift = 0;
  
  /* write input data into fifo */
  for (i=0;i<sdr->input_buffer_len;i++) {
    cbWrite(&(sdr->fifo),&sdr->input_buffer[i]);
  }

  /* Check for data in fifo */
  if (sdr->fifo.count < DAB_T_FRAME * 3) {
    // fprintf(stderr, "   no data in fifo %d < %d\n", sdr->fifo.count, DAB_T_FRAME * 3);
    return 0;
  }
  
  /* read fifo */
  sdr_read_fifo(&(sdr->fifo), DAB_T_FRAME * 2, sdr->coarse_timeshift+sdr->fine_timeshift, sdr->buffer);


  
  /* give the AGC some time to settle */
  if (sdr->startup_delay<=GAIN_SETTLE_TIME) {
    // (frame has been read and will be thrown away)
    sdr->startup_delay+=1;
    fprintf(stderr, "   startup_delay=%i\n",sdr->startup_delay);
    return 0;
  }
  


  /* complex data conversion */
  for (j=0; j<DAB_T_FRAME*2; j+=2){
    sdr->real[j/2]=sdr->buffer[j]-127;
    sdr->imag[j/2]=sdr->buffer[j+1]-127;
  }

  // unnecessary, it will be assigned afterwards
  /* /\* resetting coarse timeshift *\/ */
  /* sdr->coarse_timeshift = 0; */

  /* coarse time sync */
  /* performance bottleneck atm */
  sdr->coarse_timeshift = dab_coarse_time_sync(sdr->real,sdr->filt,sdr->force_timesync);
  // we are not in sync so -> next frame
  sdr->force_timesync=0;
  if (sdr->coarse_timeshift) {
    // fprintf(stderr, "   coarse time shift %d\n", sdr->coarse_timeshift);//
    return 0;
  }

  /* create complex frame */
  for (j=0; j<DAB_T_FRAME; j++){
    sdr->dab_frame[j][0] = sdr->real[j];
    sdr->dab_frame[j][1] = sdr->imag[j];
  }

  /* fine time sync */
  sdr->fine_timeshift = dab_fine_time_sync(sdr->dab_frame);
  if (sdr->coarse_freq_shift) { // TODO value is reset to 0 earlier in this function
    sdr->fine_timeshift = 0;
    }
  /* coarse_frequency shift */
  // fftw_plan fftw_plan_dft_1d(int n, fftw_complex *in, fftw_complex *out, int sign, unsigned flags);
  fftw_plan p;
  p = fftw_plan_dft_1d(2048, &sdr->dab_frame[2656+505+sdr->fine_timeshift], sdr->symbols[0], FFTW_FORWARD, FFTW_ESTIMATE);
  fftw_execute(p);
  fftw_destroy_plan(p);
  
  fftw_complex tmp;
  // swap upper and lower blocks
  // (fft trick where 2n real inputs are transformed as n complex ones)
    for (i = 0; i < 2048/2; i++)
    {
      // swap sdr->symbols[0][i] and sdr->symbols[0][2048/2 + i]
      tmp[0]     = sdr->symbols[0][i][0];
      tmp[1]     = sdr->symbols[0][i][1];
      sdr->symbols[0][i][0]    = sdr->symbols[0][i+2048/2][0];
      sdr->symbols[0][i][1]    = sdr->symbols[0][i+2048/2][1];
      sdr->symbols[0][i+2048/2][0] = tmp[0];
      sdr->symbols[0][i+2048/2][1] = tmp[1];
    }
  int32_t coarse_freq_shift = dab_coarse_freq_sync_2(sdr->symbols[0]);
  if (abs(coarse_freq_shift)>1) {
    // note: don't do the coarse shift immediately it tends to irreversbly run away with the frequency
    // this is handled in demod_thread_fn
    sdr->coarse_freq_shift = coarse_freq_shift;
    sdr->force_timesync = 1;
    //fprintf(stderr, "   coarse freq shift, also force timeshift %d\n", sdr->coarse_freq_shift);
    // (even if req shift not applied immediately) return anyway to do the forced time sync
    return 0;
  }

  /* fine freq correction */
  sdr->fine_freq_shift = dab_fine_freq_corr(sdr->dab_frame,sdr->fine_timeshift);

  // see https://en.wikipedia.org/wiki/Phase-shift_keying  (differential quad)
  /* d-qpsk */
  for (i=0;i<76;i++) {
    p = fftw_plan_dft_1d(2048, &sdr->dab_frame[2656+(2552*i)+504],
			 sdr->symbols[i], FFTW_FORWARD, FFTW_ESTIMATE);
    fftw_execute(p);
    fftw_destroy_plan(p);
    for (j = 0; j < 2048/2; j++)
      {
	tmp[0]     = sdr->symbols[i][j][0];
	tmp[1]     = sdr->symbols[i][j][1];
	sdr->symbols[i][j][0]    = sdr->symbols[i][j+2048/2][0];
	sdr->symbols[i][j][1]    = sdr->symbols[i][j+2048/2][1];
	sdr->symbols[i][j+2048/2][0] = tmp[0];
	sdr->symbols[i][j+2048/2][1] = tmp[1];
      }
    
  }
  //
  for (j=1;j<76;j++) {
    for (i=0;i<2048;i++)
      {
	sdr->symbols_d[j*2048+i][0] =
	  ((sdr->symbols[j][i][0]*sdr->symbols[j-1][i][0])
	   +(sdr->symbols[j][i][1]*sdr->symbols[j-1][i][1]))
	  /(sdr->symbols[j-1][i][0]*sdr->symbols[j-1][i][0]+sdr->symbols[j-1][i][1]*sdr->symbols[j-1][i][1]);
	sdr->symbols_d[j*2048+i][1] = 
	  ((sdr->symbols[j][i][0]*sdr->symbols[j-1][i][1])
	   -(sdr->symbols[j][i][1]*sdr->symbols[j-1][i][0]))
	  /(sdr->symbols[j-1][i][0]*sdr->symbols[j-1][i][0]+sdr->symbols[j-1][i][1]*sdr->symbols[j-1][i][1]);
      }
  }
  
  uint8_t* dst = tf->fic_symbols_demapped[0];
  tf->has_fic = 1;  /* Always true for SDR input */

  int k,kk;
  for (j=1;j<76;j++) {
    if (j == 4) { dst = tf->msc_symbols_demapped[0]; }
    k = 0;
    for (i=0;i<2048;i++){
      if ((i>255) && i!=1024 && i < 1793) {
        /* Frequency deinterleaving and QPSK demapping combined */  
        kk = rev_freq_deint_tab[k++];
        dst[kk] = (sdr->symbols_d[j*2048+i][0]>0)?0:1;
        dst[1536+kk] = (sdr->symbols_d[j*2048+i][1]>0)?1:0;
      }
    }
    dst += 3072;
  }
  
  //fprintf(stderr, "   ok\n");
  return 1;
}

void sdr_init(struct sdr_state_t *sdr)
{
  // circular buffer init
  cbInit(&(sdr->fifo),(DAB_T_FRAME*2*4)); // 4 frames // FIXME: sizeof??
  // malloc of various buffers
  sdr->coarse_timeshift = 0;
  sdr->fine_timeshift=0;
  sdr->dab_frame = ( fftw_complex* ) fftw_malloc( sizeof( fftw_complex ) * DAB_T_FRAME );
  sdr->prs_ifft =( fftw_complex*) fftw_malloc(sizeof(fftw_complex) * (2048 + 32));
  sdr->prs_conj_ifft = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * (2048 + 32));
  sdr->prs_syms = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * (2048 + 32));

  // malloc of various buffers
  sdr->symbols_d = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * 2048 * 76);

  /* make sure to disable fault injection by default */
  sdr->p_e_prior_dep = 0.0f;
  sdr->p_e_prior_vitdec = 0.0f;
  sdr->p_e_after_vitdec = 0.0f;
}
