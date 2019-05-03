#include "sstv.h"

/* 
 *
 * Detect VIS & frequency shift
 *
 * Each bit lasts 30 ms
 *
 */

u1_t sstv_get_vis(sstv_chan_t *e)
{
  int       selmode, ptr=0;
  int       VIS = 0, Parity = 0, HedrPtr = 0;
  const int FFTLen = 2048;
  u4_t      i=0, j=0, k=0, MaxBin = 0;
  SSTV_REAL Power[2048] = {0}, HedrBuf[100] = {0}, tone[100] = {0}, Hann[SSTV_MS_2_MAX_SAMPS(20)] = {0};
  bool      gotvis = FALSE;
  u1_t      Bit[8] = {0}, ParityBit = 0;

  for (i = 0; i < FFTLen; i++) e->fft.in[i]    = 0;

  // Create 20ms Hann window
  int samps_20ms = SSTV_MS_2_SAMPS(20);
  for (i = 0; i < samps_20ms; i++) Hann[i] = 0.5 * (1 - SSTV_MCOS( (2 * M_PI * (SSTV_REAL)i) / (samps_20ms-1) ) );

  ManualActivated = FALSE;
  
  printf("SSTV: Waiting for header\n");
  ext_send_msg_encoded(e->rx_chan, false, "EXT", "mode_name", "waiting for signal");

  int samps_10ms = SSTV_MS_2_SAMPS(10);

  while ( TRUE ) {

    if (Abort || ManualResync) return(0);

    // Read 10 ms from sound card
    sstv_pcm_read(e, samps_10ms);

    // Apply Hann window
    for (i = 0; i < samps_20ms; i++) e->fft.in[i] = e->pcm.Buffer[e->pcm.WindowPtr + i - samps_10ms] / 32768.0 * Hann[i];

    // FFT of last 20 ms
    SSTV_FFTW_EXECUTE(e->fft.Plan2048);

    // Find the bin with most power
    MaxBin = 0;
    for (i = 0; i <= GET_BIN(6000, FFTLen); i++) {
      Power[i] = POWER(e->fft.out[i]);
      if ( (i >= GET_BIN(500,FFTLen) && i < GET_BIN(3300,FFTLen)) &&
           (MaxBin == 0 || Power[i] > Power[MaxBin]))
        MaxBin = i;
    }

    // Find the peak frequency by Gaussian interpolation
    if (MaxBin > GET_BIN(500, FFTLen) && MaxBin < GET_BIN(3300, FFTLen) &&
        Power[MaxBin] > 0 && Power[MaxBin+1] > 0 && Power[MaxBin-1] > 0)
         HedrBuf[HedrPtr] = MaxBin +            (SSTV_MLOG( Power[MaxBin + 1] / Power[MaxBin - 1] )) /
                             (2 * SSTV_MLOG( pow(Power[MaxBin], 2) / (Power[MaxBin + 1] * Power[MaxBin - 1])));
    else HedrBuf[HedrPtr] = HedrBuf[(HedrPtr-1) % 45];

    // In Hertz
    HedrBuf[HedrPtr] = HedrBuf[HedrPtr] / FFTLen * sstv.nom_rate;

    // Header buffer holds 45 * 10 msec = 450 msec
    HedrPtr = (HedrPtr + 1) % 45;

    // Frequencies in the last 450 msec
    for (i = 0; i < 45; i++) tone[i] = HedrBuf[(HedrPtr + i) % 45];

    // Is there a pattern that looks like (the end of) a calibration header + VIS?
    // Tolerance ±25 Hz
    e->pic.HedrShift = 0;
    gotvis    = FALSE;
    
    for (i = 0; i < 3; i++) {
      if (e->pic.HedrShift != 0) break;
      for (j = 0; j < 3; j++) {
        if ( (tone[1*3+i]  > tone[0+j] - 25  && tone[1*3+i]  < tone[0+j] + 25)  && // 1900 Hz leader
             (tone[2*3+i]  > tone[0+j] - 25  && tone[2*3+i]  < tone[0+j] + 25)  && // 1900 Hz leader
             (tone[3*3+i]  > tone[0+j] - 25  && tone[3*3+i]  < tone[0+j] + 25)  && // 1900 Hz leader
             (tone[4*3+i]  > tone[0+j] - 25  && tone[4*3+i]  < tone[0+j] + 25)  && // 1900 Hz leader
             (tone[5*3+i]  > tone[0+j] - 725 && tone[5*3+i]  < tone[0+j] - 675) && // 1200 Hz start bit
                                                                                   // ...8 VIS bits...
             (tone[14*3+i] > tone[0+j] - 725 && tone[14*3+i] < tone[0+j] - 675)    // 1200 Hz stop bit
           ) {

          // Attempt to read VIS

          gotvis = TRUE;
          for (k = 0; k < 8; k++) {
            if      (tone[6*3+i+3*k] > tone[0+j] - 625 && tone[6*3+i+3*k] < tone[0+j] - 575) Bit[k] = 0;
            else if (tone[6*3+i+3*k] > tone[0+j] - 825 && tone[6*3+i+3*k] < tone[0+j] - 775) Bit[k] = 1;
            else { // erroneous bit
              gotvis = FALSE;
              break;
            }
          }
          if (gotvis) {
            e->pic.HedrShift = tone[0+j] - 1900;

            VIS = Bit[0] + (Bit[1] << 1) + (Bit[2] << 2) + (Bit[3] << 3) + (Bit[4] << 4) +
                 (Bit[5] << 5) + (Bit[6] << 6);
            ParityBit = Bit[7];

            printf("SSTV: VIS %d (0x%02x) @ %+d Hz\n", VIS, VIS, e->pic.HedrShift);

            Parity = Bit[0] ^ Bit[1] ^ Bit[2] ^ Bit[3] ^ Bit[4] ^ Bit[5] ^ Bit[6];

            if (VISmap[VIS] == R12BW) Parity = !Parity;

            if (Parity != ParityBit) {
              printf("SSTV: Parity fail\n");
              gotvis = FALSE;
            } else if (VISmap[VIS] == UNKNOWN) {
              printf("SSTV: Unknown VIS\n");
              gotvis = FALSE;
            } else {
                char *name = ModeSpec[VISmap[VIS]].Name;
                printf("SSTV: \"%s\"\n", name);
                ext_send_msg_encoded(e->rx_chan, false, "EXT", "mode_name", "%s", name);
                ext_send_msg_encoded(e->rx_chan, false, "EXT", "fsk_id", "");
                break;
            }
          }
        }
      }
    }

    if (gotvis) break;
    // if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(gui.tog_rx))) break;

    // Manual start
    if (ManualActivated) {

    /*
      gdk_threads_enter();
      gtk_widget_set_sensitive( gui.frame_manual, FALSE );
      gtk_widget_set_sensitive( gui.combo_card,   FALSE );
      gdk_threads_leave();
    */

      //selmode   = gtk_combo_box_get_active (GTK_COMBO_BOX(gui.combo_mode)) + 1;
      //e->pic.HedrShift = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON(gui.spin_shift));
      VIS = 0;
      for (i=0; i<0x80; i++) {
        if (VISmap[i] == selmode) {
          VIS = i;
          break;
        }
      }

      break;
    }

    if (++ptr == 10) {
      ///setVU(Power, 2048, 6, FALSE);
      ptr = 0;
    }

    e->pcm.WindowPtr += samps_10ms;
  }

  // Skip stop bit
  int stop_bit_samps = SSTV_MS_2_SAMPS(30);
  sstv_pcm_read(e, stop_bit_samps);
  e->pcm.WindowPtr += stop_bit_samps;

    if (VISmap[VIS] != UNKNOWN)
        return VISmap[VIS];
    else
        printf("SSTV: No VIS found\n");
    return 0;
}
