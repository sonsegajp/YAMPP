#include "../pad_latch.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
int main(void) {
 PadLatch latch={0}; AushimPadStatus input={0},out;
 input.buttons=0x100;pad_latch_update(&latch,&input);
 input.buttons=0;pad_latch_update(&latch,&input);
 out=pad_latch_consume(&latch);assert(out.buttons==0x100);
 out=pad_latch_consume(&latch);assert(out.buttons==0);
 input.buttons=0x200;pad_latch_update(&latch,&input);
 out=pad_latch_consume(&latch);assert(out.buttons==0x200);
 pad_latch_update(&latch,&input);input.buttons=0;pad_latch_update(&latch,&input);
 out=pad_latch_consume(&latch);assert(out.buttons==0);
 input.buttons=0x400;input.stick_x=70;pad_latch_update(&latch,&input);
 input.buttons=0;input.stick_x=-70;pad_latch_update(&latch,&input);
 out=pad_latch_consume(&latch);assert(out.buttons==0x400&&out.stick_x==-70);
 input.buttons=0x2000;pad_latch_update(&latch,&input);
 input.buttons=0;pad_latch_update(&latch,&input);
 assert(pad_latch_consume(&latch).buttons==0);
 input.buttons=0x100;pad_latch_update(&latch,&input);
 input.buttons=0;input.error=-1;pad_latch_update(&latch,&input);
 assert(pad_latch_consume(&latch).buttons==0);
 /* A trigger squeezed and let go between two frames still reaches its peak:
    that peak is what crosses the airdodge and light-shield thresholds. */
 input.error=0;input.trigger_left=0;pad_latch_update(&latch,&input);
 input.trigger_left=200;pad_latch_update(&latch,&input);
 input.trigger_left=10;pad_latch_update(&latch,&input);
 out=pad_latch_consume(&latch);assert(out.trigger_left==200);
 out=pad_latch_consume(&latch);assert(out.trigger_left==10);
 /* A held trigger is reported at its held value, never at a stale peak. */
 input.trigger_right=140;pad_latch_update(&latch,&input);
 assert(pad_latch_consume(&latch).trigger_right==140);
 input.trigger_right=60;pad_latch_update(&latch,&input);
 assert(pad_latch_consume(&latch).trigger_right==60);
 /* A disconnect drops the latched peak with the latched presses. */
 input.trigger_left=255;pad_latch_update(&latch,&input);
 input.trigger_left=0;input.error=-1;pad_latch_update(&latch,&input);
 assert(pad_latch_consume(&latch).trigger_left==0);
 puts("Short taps and trigger peaks preserved; held releases, current sticks and disconnects remain current.");
}
