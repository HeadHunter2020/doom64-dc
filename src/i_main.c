#include "i_main.h"
#include "doomdef.h"
#include "st_main.h"
#include <kos.h>
#include <kos/thread.h>
#include <kos/worker_thread.h>
#include <dc/asic.h>
#include <sys/time.h>
#include <dc/vblank.h>
#include <dc/video.h>
#include <dc/vmu_fb.h>
#include <arch/irq.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <sys/param.h>
#include <dc/maple/keyboard.h>

#include "face/AMMOLIST.xbm"

// try to claw back a few kb by not bringing in a few unused drivers
//KOS_INIT_FLAGS(INIT_CDROM | INIT_CONTROLLER | INIT_VMU | INIT_PURUPURU | INIT_IRQ | INIT_KEYBOARD | INIT_MOUSE);

void I_RumbleThread(void *param);
void I_VMUFBThread(void *param);

static uint8_t __attribute__((aligned(32))) main_stack[192*1024];
static uint8_t __attribute__((aligned(32))) ticker_stack[32*1024];

const mapped_buttons_t default_mapping = {
.map_right = {
	.n64button = PAD_RIGHT,
	.dcbuttons = {PAD_DREAMCAST_DPAD_RIGHT,0xffffffff},
	.dcused = 1
},
.map_left = {
	.n64button = PAD_LEFT,
	.dcbuttons = {PAD_DREAMCAST_DPAD_LEFT,0xffffffff},
	.dcused = 1
},
.map_up = {
	.n64button = PAD_UP,
	.dcbuttons = {PAD_DREAMCAST_DPAD_UP,0xffffffff},
	.dcused = 1
},
.map_down = {
	.n64button = PAD_DOWN,
	.dcbuttons = {PAD_DREAMCAST_DPAD_DOWN,0xffffffff},
	.dcused = 1
},
// attack
.map_attack = {
	.n64button = PAD_Z_TRIG,
	.dcbuttons = {PAD_DREAMCAST_BUTTON_A,0xffffffff},
	.dcused = 1
},
// use
.map_use = {
	.n64button = PAD_RIGHT_C,
	.dcbuttons = {PAD_DREAMCAST_BUTTON_B,0xffffffff},
	.dcused = 1
},
// automap
.map_automap = {
	.n64button = PAD_UP_C,
	.dcbuttons = {PAD_DREAMCAST_BUTTON_X,PAD_DREAMCAST_BUTTON_Y},
	.dcused = 2
},
// speed toggle
.map_speedonoff = {
	.n64button = PAD_LEFT_C,
	.dcbuttons = {0xffffffff,0xffffffff},
	.dcused = 0
},
// strafe toggle
.map_strafeonoff = {
	.n64button = PAD_DOWN_C,
	.dcbuttons = {0xffffffff,0xffffffff},
	.dcused = 0
},
// strafe left
.map_strafeleft = {
	.n64button = PAD_L_TRIG,
	.dcbuttons = {PAD_DREAMCAST_TRIGGER_L,0xffffffff},
	.dcused = 1
},
// strafe right
.map_straferight = {
	.n64button = PAD_R_TRIG,
	.dcbuttons = {PAD_DREAMCAST_TRIGGER_R,0xffffffff},
	.dcused = 1
},
.map_weaponbackward = {
	.n64button = PAD_A,
	.dcbuttons = {PAD_DREAMCAST_BUTTON_X,0xffffffff},
	.dcused = 1
},
.map_weaponforward = {
	.n64button = PAD_B,
	.dcbuttons = {PAD_DREAMCAST_BUTTON_Y,0xffffffff},
	.dcused = 1
}
};

mapped_buttons_t ingame_mapping = {
.map_right = {
	.n64button = PAD_RIGHT,
	.dcbuttons = {0xffffffff,0xffffffff},
	.dcused = 0
},
.map_left = {
	.n64button = PAD_LEFT,
	.dcbuttons = {0xffffffff,0xffffffff},
	.dcused = 0
},
.map_up = {
	.n64button = PAD_UP,
	.dcbuttons = {0xffffffff,0xffffffff},
	.dcused = 0
},
.map_down = {
	.n64button = PAD_DOWN,
	.dcbuttons = {0xffffffff,0xffffffff},
	.dcused = 0
},
// attack
.map_attack = {
	.n64button = PAD_Z_TRIG,
	.dcbuttons = {0xffffffff,0xffffffff},
	.dcused = 0
},
// use
.map_use = {
	.n64button = PAD_RIGHT_C,
	.dcbuttons = {0xffffffff,0xffffffff},
	.dcused = 0
},
// automap
.map_automap = {
	.n64button = PAD_UP_C,
	.dcbuttons = {0xffffffff,0xffffffff},
	.dcused = 0
},
// speed toggle
.map_speedonoff = {
	.n64button = PAD_LEFT_C,
	.dcbuttons = {0xffffffff,0xffffffff},
	.dcused = 0
},
// strafe toggle
.map_strafeonoff = {
	.n64button = PAD_DOWN_C,
	.dcbuttons = {0xffffffff,0xffffffff},
	.dcused = 0
},
// strafe left
.map_strafeleft = {
	.n64button = PAD_L_TRIG,
	.dcbuttons = {0xffffffff,0xffffffff},
	.dcused = 0
},
// strafe right
.map_straferight = {
	.n64button = PAD_R_TRIG,
	.dcbuttons = {0xffffffff,0xffffffff},
	.dcused = 0
},
.map_weaponbackward = {
	.n64button = PAD_A,
	.dcbuttons = {0xffffffff,0xffffffff},
	.dcused = 0
},
.map_weaponforward = {
	.n64button = PAD_B,
	.dcbuttons = {0xffffffff,0xffffffff},
	.dcused = 0
}
};

void wav_shutdown(void);

pvr_init_params_t pvr_params = { { PVR_BINSIZE_16, 0, PVR_BINSIZE_16, 0, 0 },
				TR_VERTBUF_SIZE / 2,
				1, // dma enabled
				0, // fsaa
				0, // 1 is autosort disabled
				2, // extra OPBs
				0, // Vertex buffer double-buffering enabled
 };

uint8_t __attribute__((aligned(32))) tr_buf[TR_VERTBUF_SIZE];

int side = 0;

extern int globallump;
extern int globalcm;

//----------
kthread_t *main_thread;
kthread_attr_t main_attr;

kthread_t *sys_ticker_thread;
kthread_attr_t sys_ticker_attr;

kthread_worker_t *rumble_worker_thread;
kthread_attr_t rumble_worker_attr;

kthread_worker_t *vmufb_worker_thread;
kthread_attr_t vmufb_worker_attr;

boolean disabledrawing = false;

mutex_t vbi2mtx;
condvar_t vbi2cv;

volatile int vbi2msg = 1;
atomic_int rdpmsg;
volatile s32 vsync = 0;
volatile s32 drawsync2 = 0;
volatile s32 drawsync1 = 0;

u32 NextFrameIdx = 0;

s32 ControllerPakStatus = 1;
s32 gamepad_system_busy = 0;
s32 FilesUsed = -1;
u32 SystemTickerStatus = 0;

void vblfunc(uint32_t c, void *d)
{
	(void)c;
	(void)d;
	// simulate VID_MSG_VBI handling
	vsync++;
}
extern pvr_dr_state_t dr_state;

__used void __stack_chk_fail(void) {
    unsigned int pr = (unsigned int)arch_get_ret_addr();
    printf("Stack smashed at PR=0x%08x\n", pr);
    printf("Successfully detected stack corruption!\n");
    exit(EXIT_SUCCESS);
}

int __attribute__((noreturn)) main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	dbgio_dev_select("serial");

	unsigned fpscr_start = __builtin_sh_get_fpscr();
	// mask with divbyzero/invalid exceptions enabled
	unsigned new_fpscr = fpscr_start | (1 << 10) | (1 << 11);
	__builtin_sh_set_fpscr(new_fpscr);

	global_render_state.quality = 2;
	global_render_state.fps_uncap = 1;

	vid_set_enabled(0);
	vid_set_mode(DM_640x480, PM_RGB565);
	pvr_init(&pvr_params);
	vblank_handler_add(&vblfunc, NULL);
	vid_set_enabled(1);

	pvr_set_vertbuf(PVR_LIST_TR_POLY, tr_buf, TR_VERTBUF_SIZE);

#if !D64_ERRCHECK_MUTEX
	mutex_init(&vbi2mtx, MUTEX_TYPE_NORMAL);
#else
	mutex_init(&vbi2mtx, MUTEX_TYPE_ERRORCHECK);
#endif

	cond_init(&vbi2cv);

	main_attr.create_detached = 0;
	main_attr.stack_size = 192 * 1024;
	main_attr.stack_ptr = &main_stack;
	main_attr.prio = 10;
	main_attr.label = "I_Main";

	main_thread = thd_create_ex(&main_attr, I_Main, NULL);
	dbgio_printf("started main thread\n");

	thd_join(main_thread, NULL);
	wav_shutdown();
	while (true) {
		thd_sleep(25); // don't care anymore
	}
}

void *I_Main(void *arg);
void *I_SystemTicker(void *arg);

void *I_Main(void *arg)
{
	(void)arg;
	D_DoomMain();
	return 0;
}

void *I_SystemTicker(void *arg)
{
	(void)arg;

	// this works because vbi2msg initialized to 1 before use
	while (vbi2msg) {
		thd_pass();
	}

	while (true) {
		// simulate VID_MSG_RDP handling
		if (rdpmsg) {
			rdpmsg = 0;

			SystemTickerStatus |= 16;
			thd_pass();
			continue;
		}

		if (SystemTickerStatus & 16) {
			if (demoplayback || !global_render_state.fps_uncap) {
				if ((u32)(vsync - drawsync2) < 2) {
					thd_pass();
					continue;
				}
			}

			SystemTickerStatus &= ~16;

			if (demoplayback) {
				vsync = drawsync2 + 2;
			}

			drawsync1 = vsync - drawsync2;
			drawsync2 = vsync;

#if !D64_ERRCHECK_MUTEX
			mutex_lock(&vbi2mtx);
#else
			if (mutex_lock(&vbi2mtx))
				I_Error("Failed to lock vbi2mtx in I_SystemTicker");
#endif

			vbi2msg = 1;

#if !D64_ERRCHECK_MUTEX
			cond_signal(&vbi2cv);
#else
			if (cond_signal(&vbi2cv))
				I_Error("Failed to signal vbi2cv in I_SystemTicker");
#endif

#if !D64_ERRCHECK_MUTEX
			mutex_unlock(&vbi2mtx);
#else
			if (mutex_unlock(&vbi2mtx))
					I_Error("Failed to unlock vbi2mtx in I_SystemTicker");
#endif
		}

		thd_pass();
	}

	return 0;
}

extern void S_Init(void);

vmufb_t vmubuf;
bool do_vmu_update;
extern int ArtifactLookupTable[8];
static char vmuupdbuf[32];

void I_VMUUpdateAmmo(void)
{
	static int oldammo[4];
	static int oldartifacts;
	if (players[0].artifacts != oldartifacts) {
		oldartifacts = players[0].artifacts;
		do_vmu_update = true;
	}
	else {
		for (int i = 0; i < 4; i++) {
			if (players[0].ammo[i] != oldammo[i]) {
				oldammo[i] = players[0].ammo[i];
				do_vmu_update = true;
				break;
			}
		}
	}

	if (menu_settings.VmuDisplay) {
		if (do_vmu_update) {
			const int artifactCount = ArtifactLookupTable[players[0].artifacts];
			snprintf(vmuupdbuf, sizeof(vmuupdbuf), "%03d\n%03d\n%03d\n%03d\n%d",
				players[0].ammo[am_clip], players[0].ammo[am_shell],
				players[0].ammo[am_misl], players[0].ammo[am_cell],
				artifactCount);

			vmufb_paint_xbm(&vmubuf, 1, 1, 5, 29, AMMOLIST_bits);
			vmufb_print_string_into(&vmubuf, NULL, 7, 1, 12, 31, 0, vmuupdbuf);
		}
	}
}

void I_VMUUpdateFace(uint8_t* image, int force_refresh)
{
	static uint8_t *last_image = NULL;
	if (force_refresh) {
			vmufb_clear(&vmubuf);
			if (last_image) {
				if (menu_settings.VmuDisplay == 1) // vmu face only
					vmufb_paint_xbm(&vmubuf, 9, 0, 30, 32, last_image);
				else if (menu_settings.VmuDisplay == 2) // vmu face + ammo
					vmufb_paint_xbm(&vmubuf, 18, 0, 30, 32, last_image);
			}
	}

	if (image)
		last_image = image;

	if (menu_settings.VmuDisplay == 1) // vmu face only
		vmufb_paint_xbm(&vmubuf, 9, 0, 30, 32, image);
	else if (menu_settings.VmuDisplay == 2) // vmu face + ammo
		vmufb_paint_xbm(&vmubuf, 18, 0, 30, 32, image);

	do_vmu_update = true;
}

void I_VMUFBThread(void *param)
{
	(void)param;
	maple_device_t *dev = NULL;

	// only draw to first vmu
	if ((dev = maple_enum_type(0, MAPLE_FUNC_LCD)))
		vmufb_present(&vmubuf, dev);
}

void I_VMUFB(int force_refresh)
{
	if (menu_settings.VmuDisplay == 2)
		I_VMUUpdateAmmo(); // [Striker] Update ammo display on VMU.

	if (!do_vmu_update && !force_refresh)
		return;

	thd_worker_wakeup(vmufb_worker_thread);

	do_vmu_update = false;
}

void I_RumbleThread(void *param)
{
	(void)param;
	kthread_job_t *next_job = thd_worker_dequeue_job(rumble_worker_thread);

	if (next_job) {
		uint32_t packet = (uint32_t)next_job->data;
		Z_Free(next_job);
		maple_device_t *purudev = NULL;
		purudev = maple_enum_type(0, MAPLE_FUNC_PURUPURU);
		if (purudev) {
				purupuru_rumble_raw(purudev, packet);
		}
	}
}

void I_Rumble(uint32_t packet)
{
	if ((gamemap != 33) && !demoplayback) {
		kthread_job_t *next_job = (kthread_job_t *)Z_Malloc(sizeof(kthread_job_t *), PU_STATIC, NULL);
		next_job->data = (void *)packet;

		thd_worker_add_job(rumble_worker_thread, next_job);
		thd_worker_wakeup(rumble_worker_thread);
	}
}

void I_Init(void)
{
	rumble_worker_attr.create_detached = 1;
	rumble_worker_attr.stack_size = 4096;
	rumble_worker_attr.stack_ptr = NULL;
	rumble_worker_attr.prio = PRIO_DEFAULT;
	rumble_worker_attr.label = "I_RumbleThread";
	rumble_worker_thread = thd_worker_create_ex(&rumble_worker_attr, I_RumbleThread, NULL);

	if (!rumble_worker_thread)
		I_Error("Failed to create rumble worker thread");
	else
		dbgio_printf("I_Init: started rumble worker thread\n");

	vmufb_worker_attr.create_detached = 1;
	vmufb_worker_attr.stack_size = 4096;
	vmufb_worker_attr.stack_ptr = NULL;
	vmufb_worker_attr.prio = PRIO_DEFAULT;
	vmufb_worker_attr.label = "I_VMUFBThread";

	vmufb_worker_thread = thd_worker_create_ex(&vmufb_worker_attr, I_VMUFBThread, NULL);

	if (!vmufb_worker_thread)
		I_Error("Failed to create vmufb worker thread");
	else
		dbgio_printf("I_Init: started vmufb worker thread\n");

	sys_ticker_attr.create_detached = 0;
	sys_ticker_attr.stack_size = 32768;
	sys_ticker_attr.stack_ptr = &ticker_stack;
	sys_ticker_attr.prio = 9;
	sys_ticker_attr.label = "I_SystemTicker";

	sys_ticker_thread =
		thd_create_ex(&sys_ticker_attr, I_SystemTicker, NULL);

	/* osJamMesg(&sys_msgque_vbi2, (OSMesg)VID_MSG_KICKSTART, OS_MESG_NOBLOCK); */
	// initial value must be 1 or everything deadlocks
	vbi2msg = 1;
	rdpmsg = 0;

	dbgio_printf("I_Init: started system ticker thread\n");
}

#include "stdarg.h"

int early_error = 1;


static char ieotherbuffer[256];
static char iebuffer[256];

void  __attribute__((noreturn)) __I_Error(const char *funcname, char *error, ...)
{
	va_list args;
	va_start(args, error);
	sprintf(ieotherbuffer, "%s: %s", funcname, error);
	vsprintf(iebuffer, ieotherbuffer, args);//error, args);
	va_end(args);

	pvr_scene_finish();
	pvr_wait_ready();

	if (early_error) {
#ifdef DCLOCALDEV
		dbgio_dev_select("serial");
#else
		dbgio_dev_select("fb");
#endif
		dbgio_printf("I_Error [%s]\n", iebuffer);
#ifdef DCLOCALDEV
		exit(0);
#else
		while (true) {
			;
		}
#endif
	} else {
		dbgio_dev_select("serial");
		dbgio_printf("I_Error [%s]\n", iebuffer);
#ifdef DCLOCALDEV
		exit(0);
#else
		while (true) {
			pvr_wait_ready();
			pvr_scene_begin();
			pvr_list_begin(PVR_LIST_OP_POLY);
			pvr_list_finish();
			I_ClearFrame();
			ST_Message(err_text_x, err_text_y, iebuffer, 0xffffffff, 1);
			I_DrawFrame();
			pvr_scene_finish();
		}
#endif
	}
}

typedef struct {
	int pad_data;
} pad_t;

int last_joyx;
int last_joyy;
int last_Ltrig;
int last_Rtrig;

int I_GetControllerData(void)
{
	maple_device_t *controller;
	cont_state_t *cont;
	kbd_state_t *kbd;
	mouse_state_t *mouse;
	int ret = 0;

	controller = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);

	if (controller) {
		cont = maple_dev_status(controller);

#ifdef DCLOCALDEV
		if ((cont->buttons & CONT_START) && cont->ltrig && cont->rtrig) {
			exit(0);
		}
#endif
		// START
		ret |= (cont->buttons & CONT_START) ? PAD_START : 0;

		// used for analog stick movement
		// see am_main.c, p_user.c
		last_joyx = cont->joyx;
		last_joyy = cont->joyy;
		// used for analog strafing, see p_user.c
		last_Ltrig = cont->ltrig;
		last_Rtrig = cont->rtrig;

		// second analog
		if (cont->joy2y > 10 || cont->joy2y < -10) {
			last_joyy = -cont->joy2y * 2;
		}

		if (cont->joy2x > 10) {
			last_Rtrig = MAX(last_Rtrig, cont->joy2x * 4);
			ret |= PAD_R_TRIG;
		} else if (cont->joy2x < -10) {
			last_Ltrig = MAX(last_Ltrig, -cont->joy2x * 4);
			ret |= PAD_L_TRIG;
		}

		// avoid wrap-around backward movement when flipping sign
		if (last_joyy == -128) last_joyy = -127;

		if (last_joyy)
			ret |= (((int8_t)-(last_joyy)) & 0xff);

		ret |= ((last_joyx & 0xff) << 8);

		if (!in_menu && gamemap != 33) {
			for (int i=0;i<MAP_COUNT;i++) {
				dc_n64_map_t *next_map = &(((dc_n64_map_t *)&ingame_mapping)[i]);

				if (next_map->dcused == 0) continue;

				int dcfound = 0;

				for (int j=0;j<next_map->dcused;j++) {
					switch (next_map->dcbuttons[j]) {
						case PAD_DREAMCAST_DPAD_UP:
							if (cont->buttons & CONT_DPAD_UP) {
								if (i == STRAFE_LEFT_INDEX)
									last_Ltrig = 255;
								if (i == STRAFE_RIGHT_INDEX)
									last_Rtrig = 255;
								dcfound++;
							}
							break;
						case PAD_DREAMCAST_DPAD_DOWN:
							if (cont->buttons & CONT_DPAD_DOWN) {
								if (i == STRAFE_LEFT_INDEX)
									last_Ltrig = 255;
								if (i == STRAFE_RIGHT_INDEX)
									last_Rtrig = 255;
								dcfound++;
							}
							break;
						case PAD_DREAMCAST_DPAD_LEFT:
							if (cont->buttons & CONT_DPAD_LEFT) {
								if (i == STRAFE_LEFT_INDEX)
									last_Ltrig = 255;
								if (i == STRAFE_RIGHT_INDEX)
									last_Rtrig = 255;
								dcfound++;
							}
							break;
						case PAD_DREAMCAST_DPAD_RIGHT:
							if (cont->buttons & CONT_DPAD_RIGHT) {
								if (i == STRAFE_LEFT_INDEX)
									last_Ltrig = 255;
								if (i == STRAFE_RIGHT_INDEX)
									last_Rtrig = 255;
								dcfound++;
							}
							break;
						case PAD_DREAMCAST_BUTTON_A:
							if (cont->buttons & CONT_A) {
								if (i == STRAFE_LEFT_INDEX)
									last_Ltrig = 255;
								if (i == STRAFE_RIGHT_INDEX)
									last_Rtrig = 255;
								dcfound++;
							}
							break;
						case PAD_DREAMCAST_BUTTON_B:
							if (cont->buttons & CONT_B) {
								if (i == STRAFE_LEFT_INDEX)
									last_Ltrig = 255;
								if (i == STRAFE_RIGHT_INDEX)
									last_Rtrig = 255;
								dcfound++;
							}
							break;
						case PAD_DREAMCAST_BUTTON_X:
							if (cont->buttons & CONT_X) {
								if (i == STRAFE_LEFT_INDEX)
									last_Ltrig = 255;
								if (i == STRAFE_RIGHT_INDEX)
									last_Rtrig = 255;
								dcfound++;
							}
							break;
						case PAD_DREAMCAST_BUTTON_Y:
							if (cont->buttons & CONT_Y) {
								if (i == STRAFE_LEFT_INDEX)
									last_Ltrig = 255;
								if (i == STRAFE_RIGHT_INDEX)
									last_Rtrig = 255;
								dcfound++;
							}
							break;
						case PAD_DREAMCAST_TRIGGER_L:
							if (cont->ltrig) {
								if (i != STRAFE_LEFT_INDEX)
									last_Ltrig = 255;
								dcfound++;
							}
							break;
						case PAD_DREAMCAST_TRIGGER_R:
							if (cont->rtrig) {
								if (i != STRAFE_RIGHT_INDEX)
									last_Rtrig = 255;
								dcfound++;
							}
							break;
					}
				}

				if (dcfound == next_map->dcused) {
					ret |= next_map->n64button;
				}
			}
		} else {
			// original hard-coded defaults (for menus and title map)
			// ATTACK
			ret |= (cont->buttons & (CONT_A | CONT_C)) ? PAD_Z_TRIG : 0;
			// USE
			ret |= (cont->buttons & (CONT_B | CONT_Z)) ? PAD_RIGHT_C : 0;

			// AUTOMAP is x+y together
			if ((cont->buttons & CONT_X) && (cont->buttons & CONT_Y)) {
				ret |= PAD_UP_C;
			} else {
				// WEAPON BACKWARD
				ret |= (cont->buttons & CONT_X) ? PAD_A : 0;
				// WEAPON FORWARD
				ret |= (cont->buttons & CONT_Y) ? PAD_B : 0;
			}

			// AUTOMAP select/back on 3rd paty controllers (usb4maple)
			ret |= (cont->buttons & CONT_D) ? PAD_UP_C : 0;

			// MOVE
			ret |= (cont->buttons & CONT_DPAD_RIGHT) ? PAD_RIGHT : 0;
			ret |= (cont->buttons & CONT_DPAD_LEFT) ? PAD_LEFT : 0;
			ret |= (cont->buttons & CONT_DPAD_DOWN) ? PAD_DOWN : 0;
			ret |= (cont->buttons & CONT_DPAD_UP) ? PAD_UP : 0;

			if (cont->ltrig) {
				ret |= PAD_L_TRIG;
			} else if (cont->rtrig) {
				ret |= PAD_R_TRIG;
			}
		}
	}

	// now move on to the keyboard and mouse additions
	controller = maple_enum_type(0, MAPLE_FUNC_KEYBOARD);

	if (controller) {
		kbd = maple_dev_status(controller);

		// ATTACK
		if (kbd->cond.modifiers & (KBD_MOD_LCTRL | KBD_MOD_RCTRL)) {
			ret |= PAD_Z_TRIG;
		}

		// USE
		if (kbd->cond.modifiers & (KBD_MOD_LSHIFT | KBD_MOD_RSHIFT)) {
			ret |= PAD_RIGHT_C;
		}

		for (int i = 0; i < MAX_PRESSED_KEYS; i++) {
			if (!kbd->cond.keys[i] || kbd->cond.keys[i] == KBD_KEY_ERROR) {
				break;
			}

			switch (kbd->cond.keys[i]) {
				// ATTACK
				case KBD_KEY_SPACE:
					ret |= PAD_Z_TRIG;
					break;

				// USE
				case KBD_KEY_F:
					ret |= PAD_RIGHT_C;
					break;

				// WEAPON BACKWARD
				case KBD_KEY_PGDOWN:
				case KBD_KEY_PAD_MINUS:
					ret |= PAD_A;
					break;

				// WEAPON FORWARD
				case KBD_KEY_PGUP:
				case KBD_KEY_PAD_PLUS:
					ret |= PAD_B;
					break;

				// MOVE
				case KBD_KEY_D: // RIGHT
				case KBD_KEY_RIGHT:
					ret |= PAD_RIGHT;
					break;

				case KBD_KEY_A: // LEFT
				case KBD_KEY_LEFT:
					ret |= PAD_LEFT;
					break;

				case KBD_KEY_S: // DOWN
				case KBD_KEY_DOWN:
					ret |= PAD_DOWN;
					break;

				case KBD_KEY_W: // UP
				case KBD_KEY_UP:
					ret |= PAD_UP;
					break;

				// START
				case KBD_KEY_ESCAPE:
				case KBD_KEY_ENTER:
				case KBD_KEY_PAD_ENTER:
					ret |= PAD_START;
					break;

				// MAP
				case KBD_KEY_TAB:
					ret |= PAD_UP_C;
					break;

				// STRAFE
				case KBD_KEY_COMMA: // L
					ret |= PAD_L_TRIG;
					last_Ltrig = 255;
					break;

				case KBD_KEY_PERIOD: // R
					ret |= PAD_R_TRIG;
					last_Rtrig = 255;
					break;

				case KBD_KEY_BACKSPACE:
					ret |= PAD_LEFT_C;
					break;

				default:
			}
		}
	}

	controller = maple_enum_type(0, MAPLE_FUNC_MOUSE);

	if (controller) {
		mouse = maple_dev_status(controller);

		// ATTACK
		if (mouse->buttons & MOUSE_LEFTBUTTON) {
			ret |= PAD_Z_TRIG;
		}

		// USE
		if (mouse->buttons & MOUSE_RIGHTBUTTON) {
			ret |= PAD_RIGHT_C;
		}

		// START
		if (mouse->buttons & MOUSE_SIDEBUTTON) {
			ret |= PAD_START;
		}

		// STRAFE
		// Only five buttons mouse, supported by usb4maple
		if (mouse->buttons & (1 << 5)) { // BACKWARD
			ret |= PAD_L_TRIG;
			last_Ltrig = 255;
		}
		if (mouse->buttons & (1 << 4)) { // FORWARD
			ret |= PAD_R_TRIG;
			last_Rtrig = 255;
		}

		// WEAPON
		if (mouse->dz < 0) {
			ret |= PAD_A;
		} else if (mouse->dz > 0) {
			ret |= PAD_B;
		}

		if (mouse->dx) {
			last_joyx = mouse->dx*4;

			if (last_joyx > 127) {
				last_joyx = 127;
			} else if(last_joyx < -128) {
				last_joyx = -128;
			}

			ret = (ret & ~0xFF00) | ((last_joyx & 0xff) << 8);
		}

		if (mouse->dy) {
			last_joyy = -mouse->dy*4;

			if (last_joyy > 127) {
				last_joyy = 127;
			} else if(last_joyy < -128) {
				last_joyy = -128;
			}

			ret = (ret & ~0xFF)   |  (last_joyy & 0xff);
		}
	}

	return ret;
}

void I_ClearFrame(void) // 8000637C
{
	NextFrameIdx += 1;

	globallump = -1;
	globalcm = -2;
}

void I_DrawFrame(void) // 80006570
{
#if !D64_ERRCHECK_MUTEX
	mutex_lock(&vbi2mtx);
#else
	if (mutex_lock(&vbi2mtx))
		I_Error("Failed to lock vbi2mtx in I_DrawFrame");
#endif

#if !D64_ERRCHECK_MUTEX
	while (!vbi2msg)
		cond_wait(&vbi2cv, &vbi2mtx);
#else
	while (!vbi2msg)
		if (cond_wait(&vbi2cv, &vbi2mtx))
			I_Error("Failed to wait on vbi2v in I_DrawFrame");
#endif

	vbi2msg = 0;

#if !D64_ERRCHECK_MUTEX
	mutex_unlock(&vbi2mtx);
#else
	if (mutex_unlock(&vbi2mtx))
			I_Error("Failed to unlock vbi2mtx in I_DrawFrame");
#endif
}

short SwapShort(short dat)
{
	return ((((dat << 8) | (dat >> 8 & 0xff)) << 16) >> 16);
}

#define MELTALPHA2 0.00392f
#define FB_TEX_W 512
#define FB_TEX_H 256
#define FB_TEX_SIZE (FB_TEX_W * FB_TEX_H * sizeof(uint16_t))

extern float empty_table[129];
extern void P_FlushAllCached(void);

static pvr_vertex_t __attribute__((aligned(32))) wipeverts[8];
static 	pvr_poly_cxt_t wipecxt;
static pvr_poly_hdr_t __attribute__((aligned(32))) wipehdr;

void I_WIPE_MeltScreen(void)
{
	pvr_ptr_t pvrfb = 0;
	pvr_vertex_t *vert;

	float x0, y0, x1, y1;
	float y0a, y1a;
	float u0, v0, u1, v1;
	float v1a;

	uint32_t save;
	uint16_t *fb = (uint16_t *)Z_Malloc(FB_TEX_SIZE, PU_STATIC, NULL);

	pvr_wait_ready();

	P_FlushAllCached();
	pvrfb = pvr_mem_malloc(FB_TEX_SIZE);
	if (!pvrfb) {
		I_Error("PVR OOM for melt fb");
	}

	memset(fb, 0, FB_TEX_SIZE);

	save = irq_disable();
	for (uint32_t y = 0; y < 480; y += 2) {
		for (uint32_t x = 0; x < 640; x += 2) {
			// (y/2) * 512 == y << 8
			// y*640 == (y<<9) + (y<<7)
			fb[(y << 8) + (x >> 1)] =
				vram_s[((y << 9) + (y << 7)) + x];
		}
	}
	irq_restore(save);

	pvr_txr_load(fb, pvrfb, FB_TEX_SIZE);
	pvr_poly_cxt_txr(&wipecxt, PVR_LIST_TR_POLY,
			 PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED, FB_TEX_W,
			 FB_TEX_H, pvrfb, PVR_FILTER_NONE);
	wipecxt.blend.src = PVR_BLEND_ONE;
	wipecxt.blend.dst = PVR_BLEND_ONE;
	pvr_poly_compile(&wipehdr, &wipecxt);

	// Fill borders with black
	pvr_set_bg_color(0, 0, 0);
	pvr_fog_table_color(0.0f, 0.0f, 0.0f, 0.0f);
	pvr_fog_table_custom(empty_table);

	u0 = 0.0f;
	u1 = 0.625f; // 320.0f / 512.0f;
	v0 = 0.0f;
	v1 = 0.9375f; // 240.0f / 256.0f;
	x0 = 0.0f;
	y0 = 0.0f;
	x1 = 640;
	y1 = 480;
	y0a = y0;
	y1a = y1;

	for (int vn = 0; vn < 4; vn++) {
		wipeverts[vn].flags = PVR_CMD_VERTEX;
		wipeverts[vn].z = 5.0f;
		wipeverts[vn].argb = 0xffff0000; // red, alpha 255/255
	}
	wipeverts[3].flags = PVR_CMD_VERTEX_EOL;

	for (int vn = 4; vn < 8; vn++) {
		wipeverts[vn].flags = PVR_CMD_VERTEX;
		wipeverts[vn].z = 5.01f;
		wipeverts[vn].argb = 0x10080808; // almost black, alpha 16/255
	}
	wipeverts[7].flags = PVR_CMD_VERTEX_EOL;

	for (int i = 0; i < 160; i += 2) {
		pvr_scene_begin();
		pvr_list_begin(PVR_LIST_OP_POLY);
		pvr_list_finish();
		vert = wipeverts;
		vert->x = x0;
		vert->y = y1;
		vert->u = u0;
		vert->v = v1;
		vert++;

		vert->x = x0;
		vert->y = y0;
		vert->u = u0;
		vert->v = v0;
		vert++;

		vert->x = x1;
		vert->y = y1;
		vert->u = u1;
		vert->v = v1;
		vert++;

		vert->x = x1;
		vert->y = y0;
		vert->u = u1;
		vert->v = v0;
		vert++;

#if 1
		// I'm not sure if I need this but leaving it for now
		if (y1a > y1 + 31) {
			double ydiff = y1a - 480;
			y1a = 480;
			v1a = (240.0f - ydiff) / 256.0f;
		} else {
			v1a = v1;
		}
#endif

		vert->x = x0;
		vert->y = y1a;
		vert->u = u0;
		vert->v = v1a;
		vert++;

		vert->x = x0;
		vert->y = y0a;
		vert->u = u0;
		vert->v = v0;
		vert++;

		vert->x = x1;
		vert->y = y1a;
		vert->u = u1;
		vert->v = v1a;
		vert++;

		vert->x = x1;
		vert->y = y0a;
		vert->u = u1;
		vert->v = v0;

		pvr_list_prim(PVR_LIST_TR_POLY, &wipehdr,
			      sizeof(pvr_poly_hdr_t));
		pvr_list_prim(PVR_LIST_TR_POLY, wipeverts,
			      8 * sizeof(pvr_vertex_t));
		pvr_scene_finish();
		pvr_wait_ready();

		// after PVR changes, have to submit twice
		// or it flickers
		pvr_scene_begin();
		pvr_list_begin(PVR_LIST_OP_POLY);
		pvr_list_finish();
		pvr_list_prim(PVR_LIST_TR_POLY, &wipehdr,
			      sizeof(pvr_poly_hdr_t));
		pvr_list_prim(PVR_LIST_TR_POLY, wipeverts,
			      8 * sizeof(pvr_vertex_t));
		pvr_scene_finish();
		pvr_wait_ready();

		if (i < 158) {
			save = irq_disable();
			for (uint32_t y = 0; y < 480; y += 2) {
				for (uint32_t x = 0; x < 640; x += 2) {
					// (y/2) * 512 == y << 8
					// y*640 == (y<<9) + (y<<7)
					fb[(y << 8) + (x >> 1)] =
						vram_s[((y << 9) + (y << 7)) +
						       x];
				}
			}
			irq_restore(save);

			pvr_txr_load(fb, pvrfb, FB_TEX_SIZE);

			y0a += 1.0f;
			y1a += 1.0f;
		}
	}

	pvr_mem_free(pvrfb);

	Z_Free(fb);
	I_WIPE_FadeOutScreen();
	return;
}

void I_WIPE_FadeOutScreen(void)
{
	pvr_ptr_t pvrfb = 0;
	pvr_vertex_t *vert;

	float x0, y0, x1, y1;
	float u0, v0, u1, v1;

	uint32_t save;
	uint16_t *fb = (uint16_t *)Z_Malloc(FB_TEX_SIZE, PU_STATIC, NULL);

	pvr_wait_ready();

	P_FlushAllCached();
	pvrfb = pvr_mem_malloc(FB_TEX_SIZE);
	if (!pvrfb) {
		I_Error("PVR OOM for melt fb");
	}

	memset(fb, 0, FB_TEX_SIZE);

	save = irq_disable();
	for (uint32_t y = 0; y < 480; y += 2) {
		for (uint32_t x = 0; x < 640; x += 2) {
			// (y/2) * 512 == y << 8
			// y*640 == (y<<9) + (y<<7)
			fb[(y << 8) + (x >> 1)] =
				vram_s[((y << 9) + (y << 7)) + x];
		}
	}
	irq_restore(save);

	pvr_txr_load(fb, pvrfb, FB_TEX_SIZE);
	pvr_poly_cxt_txr(&wipecxt, PVR_LIST_TR_POLY,
			 PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED, FB_TEX_W,
			 FB_TEX_H, pvrfb, PVR_FILTER_NONE);
	wipecxt.blend.src = PVR_BLEND_ONE;
	wipecxt.blend.dst = PVR_BLEND_ONE;
	pvr_poly_compile(&wipehdr, &wipecxt);

	pvr_set_bg_color(0, 0, 0);
	pvr_fog_table_color(0.0f, 0.0f, 0.0f, 0.0f);

	u0 = 0.0f;
	u1 = 0.625f; // 320.0f / 512.0f;
	v0 = 0.0f;
	v1 = 0.9375f; // 240.0f / 256.0f;
	x0 = 0.0f;
	y0 = 0.0f;
	x1 = 640;
	y1 = 480;
	for (int vn = 0; vn < 4; vn++) {
		wipeverts[vn].flags = PVR_CMD_VERTEX;
		wipeverts[vn].z = 5.0f;
	}
	wipeverts[3].flags = PVR_CMD_VERTEX_EOL;

	for (int i = 248; i >= 0; i -= 8) {
		uint8_t ui = (uint8_t)(i & 0xff);
		uint32_t fcol = 0xff000000 | (ui << 16) | (ui << 8) | ui;

		pvr_scene_begin();
		pvr_list_begin(PVR_LIST_OP_POLY);
		pvr_list_finish();
		vert = wipeverts;
		vert->x = x0;
		vert->y = y1;
		vert->u = u0;
		vert->v = v1;
		vert->argb = fcol;
		vert++;

		vert->x = x0;
		vert->y = y0;
		vert->u = u0;
		vert->v = v0;
		vert->argb = fcol;
		vert++;

		vert->x = x1;
		vert->y = y1;
		vert->u = u1;
		vert->v = v1;
		vert->argb = fcol;
		vert++;

		vert->x = x1;
		vert->y = y0;
		vert->u = u1;
		vert->v = v0;
		vert->argb = fcol;

		pvr_list_prim(PVR_LIST_TR_POLY, &wipehdr,
			      sizeof(pvr_poly_hdr_t));
		pvr_list_prim(PVR_LIST_TR_POLY, wipeverts, 4 * sizeof(pvr_vertex_t));
		pvr_scene_finish();
		pvr_wait_ready();
	}

	pvr_mem_free(pvrfb);

	Z_Free(fb);
	return;
}

#include <dc/vmu_pkg.h>

s32 Pak_Memory = 0;
s32 Pak_Size = 0;
u8 *Pak_Data;
dirent_t __attribute__((aligned(32))) FileState[200];

unsigned short vmu_icon_pal[] = {
0xF404, 0xF303, 0xF202, 0xF101, 0xFF62, 0xFE41, 0xFB42, 0xF732, 0xFC31, 0xF610, 0xF721, 0xF921, 0xF842, 0xFA21, 0xFA55, 0xF510, };

unsigned char vmu_icon_img[] = {
0x23,0x32,0x21,0x22,0x2B,0x8B,0x31,0x22,0x22,0x23,0xAA,0xF3,0x33,0x22,0x23,0x21,
0x23,0x93,0x32,0x2F,0xBD,0xA3,0x33,0x33,0x33,0x33,0xFD,0x8A,0xF3,0x23,0x3F,0x31,
0x23,0xF7,0x73,0x2A,0xDD,0x33,0xCC,0xAA,0xAA,0x73,0x33,0x8D,0x93,0x3F,0x9F,0x31,
0x23,0xFF,0xA7,0x78,0xDA,0xCC,0xAF,0xFF,0xFF,0x9C,0x79,0xB8,0x8F,0xF9,0xAF,0x21,
0x22,0x3F,0xA7,0x9D,0xDA,0x99,0xFF,0x9F,0xFF,0x9F,0xF9,0x78,0x8A,0x99,0xF3,0x20,
0x21,0x23,0x99,0xA8,0xDF,0x9F,0xFF,0x33,0x33,0x99,0x99,0x98,0x56,0x7A,0xF2,0x00,
0x21,0x13,0xF9,0x9B,0xBF,0xF3,0x33,0x33,0x33,0x33,0x39,0xFB,0x8B,0x99,0x32,0x11,
0x21,0x12,0x37,0xA8,0xDF,0xFF,0x33,0x39,0xF3,0x33,0x97,0x98,0x8B,0x99,0x31,0x11,
0x22,0x12,0x33,0xCA,0x8A,0xFA,0x9F,0x3E,0xD3,0xFC,0x7A,0xA5,0xB9,0xA2,0x21,0x11,
0x22,0x23,0xE7,0xAD,0xAD,0xFF,0x99,0xFD,0xDA,0xC7,0x9A,0xDC,0xB7,0x79,0x21,0x11,
0x12,0x33,0xCA,0xF8,0x5A,0xF9,0x99,0xFD,0xD9,0xAA,0xAA,0x74,0x89,0x77,0x22,0x12,
0x12,0x3C,0x7A,0xFD,0x88,0xA9,0xAA,0xFB,0xDF,0xAB,0xAC,0x68,0x8A,0x77,0x92,0x22,
0x12,0x3E,0x7A,0x39,0xA5,0x8A,0xBB,0x9B,0xD9,0xBB,0xB8,0x4B,0xB2,0x77,0xA2,0x22,
0x12,0x3E,0x79,0x3F,0xA5,0xDB,0xBD,0x98,0xD9,0xDD,0xD6,0x4B,0x92,0xE7,0xA2,0x12,
0x22,0x3E,0xA9,0x33,0x98,0x8A,0xBB,0x7B,0x8F,0xB6,0xC8,0x4B,0x22,0xE7,0xB2,0x01,
0x23,0x3C,0x79,0x33,0x9A,0x58,0xDA,0xA8,0x5F,0x76,0x44,0x6A,0x22,0xE7,0xB2,0x01,
0x32,0x37,0x79,0x37,0xCF,0x54,0x5D,0xB5,0x5A,0x84,0x44,0xAA,0x62,0xE7,0x72,0x11,
0x22,0x37,0x77,0xC6,0xBC,0x98,0x44,0x85,0x48,0x44,0x48,0xA6,0xBE,0xC7,0xA2,0x01,
0x22,0x3E,0x77,0xBD,0x8C,0xFF,0xB4,0x45,0x54,0x45,0xFF,0xB5,0x66,0xE7,0xA2,0x10,
0x23,0xEE,0x7B,0xB8,0x8A,0x9A,0x9B,0x48,0x54,0x89,0xAF,0x85,0x8C,0xC7,0x7E,0x21,
0x3E,0xE7,0x7A,0xAA,0xAA,0xA8,0xAF,0xD8,0xD8,0xFD,0x8D,0x66,0x6B,0x77,0x7C,0xE2,
0xAB,0x9A,0x9B,0xB9,0xF9,0x9C,0xDD,0x8F,0xA5,0x88,0xA9,0x99,0x76,0xB9,0x99,0x9C,
0x33,0x33,0x3A,0xDD,0x63,0x3C,0xAB,0x95,0x4A,0xDA,0xF2,0x2C,0x66,0xA2,0x21,0x22,
0x11,0x11,0x22,0xA8,0x8C,0x33,0xC9,0xDD,0xD5,0xAB,0x22,0x74,0x5D,0x21,0x00,0x00,
0x01,0x11,0x01,0x2D,0x55,0xE7,0x89,0xF9,0xA9,0xA8,0x9C,0x45,0xD2,0x10,0x00,0x00,
0x00,0x10,0x00,0x12,0x85,0x58,0xB5,0x8A,0xBD,0x5A,0x45,0x48,0x21,0x01,0x11,0x00,
0x00,0x00,0x00,0x12,0x2D,0x54,0x8A,0x5B,0xD5,0x88,0x45,0xD3,0x21,0x11,0x11,0x00,
0x00,0x00,0x00,0x00,0x12,0x2D,0x8A,0x5D,0x85,0xB8,0xD3,0x32,0x22,0x21,0x11,0x11,
0x10,0x11,0x11,0x00,0x01,0x23,0x33,0xAD,0xDA,0x33,0x32,0x22,0x33,0x21,0x01,0x22,
0x11,0x11,0x11,0x00,0x10,0x12,0x12,0xF9,0x9F,0x31,0x12,0x22,0x22,0x11,0x01,0x22,
0x10,0x10,0x00,0x00,0x00,0x11,0x12,0x39,0x93,0x32,0x22,0x22,0x22,0x22,0x22,0x22,
0x10,0x00,0x00,0x00,0x00,0x00,0x11,0x3F,0xF3,0x22,0x32,0x22,0x22,0x22,0x32,0x22,
};

int I_CheckControllerPak(void)
{
	maple_device_t *vmudev = NULL;

	ControllerPakStatus = 0;
	FilesUsed = -1;

	vmudev = maple_enum_type(0, MAPLE_FUNC_MEMCARD);
	if (!vmudev) return PFS_ERR_NOPACK;

	file_t d;
	dirent_t *de;

	d = fs_open("/vmu/a1", O_RDONLY | O_DIR);
	if(!d) return PFS_ERR_ID_FATAL;

	Pak_Memory = 200;

	memset(FileState, 0, sizeof(dirent_t)*200);

	FilesUsed = 0;
	int FileCount = 0;
	while (NULL != (de = fs_readdir(d))) {
		if (strcmp(de->name, ".") == 0) continue;
		if (strcmp(de->name, "..") == 0) continue;
		memcpy(&FileState[FileCount++], de, sizeof(dirent_t));			
		Pak_Memory -= (de->size / 512);
		FilesUsed += 1;
	}

	fs_close(d);

	ControllerPakStatus = 1;

	return 0;
}

static char full_fn[512];

int I_DeletePakFile(dirent_t *de)
{
	maple_device_t *vmudev = NULL;
	int blocksize;
	blocksize = de->size >> 9;
	sprintf(full_fn, "/vmu/a1/%s", de->name);

	ControllerPakStatus = 0;

	vmudev = maple_enum_type(0, MAPLE_FUNC_MEMCARD);
	if (!vmudev)
		return PFS_ERR_NOPACK;

	int rv = fs_unlink(full_fn);
	if(rv) return PFS_ERR_ID_FATAL;

	FilesUsed -= 1;
	Pak_Memory += blocksize;
	ControllerPakStatus = 1;

	return 0;
}

static vmu_pkg_t pkg;

int I_SavePakSettings(doom64_settings_t *msettings)
{
	uint8 *pkg_out;
	ssize_t pkg_size;
	maple_device_t *vmudev = NULL;

	vmudev = maple_enum_type(0, MAPLE_FUNC_MEMCARD);
	if (!vmudev) return PFS_ERR_NOPACK;

	file_t d = fs_open("/vmu/a1/doom64stg", O_WRONLY | O_CREAT);
	if(!d) return PFS_ERR_ID_FATAL;

	memset(&pkg, 0, sizeof(vmu_pkg_t));
	strcpy(pkg.desc_short,"D64 settings");
	strcpy(pkg.desc_long, "Doom 64 settings data");
	strcpy(pkg.app_id, "Doom 64");
	pkg.icon_cnt = 1;
	pkg.icon_data = vmu_icon_img;
	memcpy(pkg.icon_pal, vmu_icon_pal, sizeof(vmu_icon_pal));
	pkg.data_len = 384;
	// doesn't matter, just not NULL
	pkg.data = vmu_icon_img;

	vmu_pkg_build(&pkg, &pkg_out, &pkg_size);

	if(!pkg_out || pkg_size <= 0) {
		return PFS_ERR_ID_FATAL;
	}

	memcpy(&pkg_out[640], msettings, sizeof(doom64_settings_t));

	ssize_t rv = fs_write(d, pkg_out, pkg_size);
	fs_close(d);
	free(pkg_out);

	if (rv == pkg_size) {
		return 0;
	} else {
		return PFS_ERR_ID_FATAL;
	}
}

int I_SavePakFile(void)
{
	uint8 *pkg_out;
	ssize_t pkg_size;
	maple_device_t *vmudev = NULL;

	ControllerPakStatus = 0;

	vmudev = maple_enum_type(0, MAPLE_FUNC_MEMCARD);
	if (!vmudev) return PFS_ERR_NOPACK;

	file_t d = fs_open("/vmu/a1/doom64", O_WRONLY);
	if(!d) return PFS_ERR_ID_FATAL;

	memset(&pkg, 0, sizeof(vmu_pkg_t));
	strcpy(pkg.desc_short,"Doom 64 saves");
	strcpy(pkg.desc_long, "Doom 64 save data");
	strcpy(pkg.app_id, "Doom 64");
	pkg.icon_cnt = 1;
	pkg.icon_data = vmu_icon_img;
	memcpy(pkg.icon_pal, vmu_icon_pal, sizeof(vmu_icon_pal));
	pkg.data_len = 512;
	if (!Pak_Data) {
		return PFS_ERR_ID_FATAL;
	}
	pkg.data = Pak_Data;

	vmu_pkg_build(&pkg, &pkg_out, &pkg_size);
	if(!pkg_out || pkg_size <= 0) {
		return PFS_ERR_ID_FATAL;
	}
	ssize_t rv = fs_write(d, pkg_out, pkg_size);
	fs_close(d);
	free(pkg_out);

	if (rv == pkg_size) {
		ControllerPakStatus = 1;
		return 0;
	} else {
	    return PFS_ERR_ID_FATAL;
	}
}

#define COMPANY_CODE 0x3544 // 5D
#define GAME_CODE 0x4e444d45 // NDME

int I_ReadPakSettings(doom64_settings_t *msettings)
{
	ssize_t size;
	maple_device_t *vmudev = NULL;
	uint8_t *data;

	vmudev = maple_enum_type(0, MAPLE_FUNC_MEMCARD);
	if (!vmudev) return PFS_ERR_NOPACK;

	file_t d = fs_open("/vmu/a1/doom64stg", O_RDONLY);
	if(!d) return PFS_ERR_ID_FATAL;

	size = fs_total(d);
	data = calloc(1, size);

	if(!data) {
		fs_close(d);
		return PFS_ERR_ID_FATAL;
	}

	// read version first
	ssize_t res = fs_read(d, data, size);
	fs_close(d);

	if (res <= 0) {
		free(data);
		return PFS_ERR_ID_FATAL;
	}

	int save_version = *(int *)(&data[640]);
	int save_size = sizeof(doom64_settings_t);
	if (save_version > 20) {
		// this is very likely hud opacity, so error out here
		free(data);
		return PFS_ERR_ID_FATAL;
	}

	// don't try to read junk data from older save
	if (SETTINGS_SAVE_VERSION > save_version)
		save_size -= (sizeof(int) * (SETTINGS_SAVE_VERSION - save_version));

	memcpy(msettings, &data[640], save_size);

	msettings->runintroduction = false;
	global_render_state.quality = msettings->Quality;
	global_render_state.fps_uncap = msettings->FpsUncap;

	// currently on SETTINGS_SAVE_VERSION == 3
	// ending on VmuDisplay
	if (SETTINGS_SAVE_VERSION > msettings->version) {
		if (msettings->version == 2)
			msettings->VmuDisplay = 0;
		else if (msettings->version == 1)
			msettings->Interpolate = 0;
	}

	free(data);

	return 0;
}

int I_ReadPakFile(void)
{
	ssize_t size;
	maple_device_t *vmudev = NULL;
	uint8_t *data;

	ControllerPakStatus = 0;

	vmudev = maple_enum_type(0, MAPLE_FUNC_MEMCARD);
	if (!vmudev) return PFS_ERR_NOPACK;

	Pak_Data = NULL;
	Pak_Size = 0;

	file_t d = fs_open("/vmu/a1/doom64", O_RDONLY);
	if(!d) return PFS_ERR_ID_FATAL;

	size = fs_total(d);
	data = calloc(1, size);

	if(!data) {
		fs_close(d);
		return PFS_ERR_ID_FATAL;
	}

	memset(&pkg, 0, sizeof(pkg));
	ssize_t res = fs_read(d, data, size);
	fs_close(d);

	if (res <= 0) {
		free(data);
		return PFS_ERR_ID_FATAL;
	}

	if(vmu_pkg_parse(data, &pkg) < 0) {
		free(data);
		return PFS_ERR_ID_FATAL;
	}

	Pak_Size = 512;
	Pak_Data = (byte *)Z_Malloc(Pak_Size, PU_STATIC, NULL);
	memset(Pak_Data, 0, Pak_Size);
	memcpy(Pak_Data, pkg.data, Pak_Size);
	ControllerPakStatus = 1;

	free(data);

	return 0;
}

int I_CreatePakFile(void)
{
	uint8 *pkg_out;
	ssize_t pkg_size;
	maple_device_t *vmudev = NULL;

	ControllerPakStatus = 0;

	vmudev = maple_enum_type(0, MAPLE_FUNC_MEMCARD);
	if (!vmudev) return PFS_ERR_NOPACK;

	memset(&pkg, 0, sizeof(vmu_pkg_t));
	strcpy(pkg.desc_short,"Doom 64 saves");
	strcpy(pkg.desc_long, "Doom 64 save data");
	strcpy(pkg.app_id, "Doom 64");
	pkg.icon_cnt = 1;
	pkg.icon_data = vmu_icon_img;
	memcpy(pkg.icon_pal, vmu_icon_pal, sizeof(vmu_icon_pal));
	pkg.data_len = 512;
	Pak_Size = 512;
	Pak_Data = (byte *)Z_Malloc(Pak_Size, PU_STATIC, NULL);
	memset(Pak_Data, 0, Pak_Size);
	pkg.data = Pak_Data;

	file_t d = fs_open("/vmu/a1/doom64", O_RDWR | O_CREAT);
	if(!d) return PFS_ERR_ID_FATAL;

	vmu_pkg_build(&pkg, &pkg_out, &pkg_size);
	if(!pkg_out || pkg_size <= 0) {
		return PFS_ERR_ID_FATAL;
	}
	ssize_t rv = fs_write(d, pkg_out, pkg_size);
	fs_close(d);
	free(pkg_out);

	if (rv == pkg_size) {
		ControllerPakStatus = 1;
		return 0;
	} else {
	    return PFS_ERR_ID_FATAL;
	}
}

void update_map(int dcused, char dcbuts[2][32], dc_n64_map_t *mapping)
{
	mapping->dcused = dcused;
	if (dcused == 0) {
		mapping->dcbuttons[0] = 0xffffffff;
		mapping->dcbuttons[1] = 0xffffffff;
	} else {
		mapping->dcbuttons[1] = 0xffffffff;

		if (!strncmp("BUTTON_A",dcbuts[0],8)) {
			mapping->dcbuttons[0] = PAD_DREAMCAST_BUTTON_A;
		} else if (!strncmp("BUTTON_B",dcbuts[0],8)) {
			mapping->dcbuttons[0] = PAD_DREAMCAST_BUTTON_B;
		} else if (!strncmp("BUTTON_X",dcbuts[0],8)) {
			mapping->dcbuttons[0] = PAD_DREAMCAST_BUTTON_X;
		} else if (!strncmp("BUTTON_Y",dcbuts[0],8)) {
			mapping->dcbuttons[0] = PAD_DREAMCAST_BUTTON_Y;
		} else if (!strncmp("TRIGGER_L",dcbuts[0],9)) {
			mapping->dcbuttons[0] = PAD_DREAMCAST_TRIGGER_L;
		} else if (!strncmp("TRIGGER_R",dcbuts[0],9)) {
			mapping->dcbuttons[0] = PAD_DREAMCAST_TRIGGER_R;
		} else if (!strncmp("DPAD_LEFT",dcbuts[0],9)) {
			mapping->dcbuttons[0] = PAD_DREAMCAST_DPAD_LEFT;
		} else if (!strncmp("DPAD_RIGHT",dcbuts[0],10)) {
			mapping->dcbuttons[0] = PAD_DREAMCAST_DPAD_RIGHT;
		} else if (!strncmp("DPAD_UP",dcbuts[0],7)) {
			mapping->dcbuttons[0] = PAD_DREAMCAST_DPAD_UP;
		} else if (!strncmp("DPAD_DOWN",dcbuts[0],9)) {
			mapping->dcbuttons[0] = PAD_DREAMCAST_DPAD_DOWN;
		}

		if (dcused == 2) {
			if (!strncmp("BUTTON_A",dcbuts[1],8)) {
				mapping->dcbuttons[1] = PAD_DREAMCAST_BUTTON_A;
			} else if (!strncmp("BUTTON_B",dcbuts[1],8)) {
				mapping->dcbuttons[1] = PAD_DREAMCAST_BUTTON_B;
			} else if (!strncmp("BUTTON_X",dcbuts[1],8)) {
				mapping->dcbuttons[1] = PAD_DREAMCAST_BUTTON_X;
			} else if (!strncmp("BUTTON_Y",dcbuts[1],8)) {
				mapping->dcbuttons[1] = PAD_DREAMCAST_BUTTON_Y;
			} else if (!strncmp("TRIGGER_L",dcbuts[1],9)) {
				mapping->dcbuttons[1] = PAD_DREAMCAST_TRIGGER_L;
			} else if (!strncmp("TRIGGER_R",dcbuts[1],9)) {
				mapping->dcbuttons[1] = PAD_DREAMCAST_TRIGGER_R;
			} else if (!strncmp("DPAD_LEFT",dcbuts[1],9)) {
				mapping->dcbuttons[1] = PAD_DREAMCAST_DPAD_LEFT;
			} else if (!strncmp("DPAD_RIGHT",dcbuts[1],10)) {
				mapping->dcbuttons[1] = PAD_DREAMCAST_DPAD_RIGHT;
			} else if (!strncmp("DPAD_UP",dcbuts[1],7)) {
				mapping->dcbuttons[1] = PAD_DREAMCAST_DPAD_UP;
			} else if (!strncmp("DPAD_DOWN",dcbuts[1],9)) {
				mapping->dcbuttons[1] = PAD_DREAMCAST_DPAD_DOWN;
			}
		}
	}
}

static const char outer_delimiters[] = "\n";
static const char inner_delimiters[] = ",";
static char n64_control[32];
static char dcbuts[2][32];

void I_ParseMappingFile(char *mapping_file)
{
	if (mapping_file) {
		//dbgio_printf("parsing: %s\n", mapping_file);

		int lineno = 0;

		char* token;
		char* outer_saveptr = NULL;
		char* inner_saveptr = NULL;

		token = strtok_r(mapping_file, outer_delimiters, &outer_saveptr);

		while (token != NULL) {
			lineno++;
			//dbgio_printf("next line: %s\n", token);
			int inner_encountered = 0;
			char* inner_token = strtok_r(token, inner_delimiters, &inner_saveptr);

			int dcused = 0;

			strncpy(n64_control, "INVALID", 8);
			strncpy(dcbuts[0], "0xffffffff", 11);
			strncpy(dcbuts[1], "0xffffffff", 11);

			while (inner_token != NULL) {
				inner_encountered++;
				//dbgio_printf("\t%s\n", inner_token);
				if (inner_encountered == 1) {
					strcpy(n64_control, inner_token);
				} else if (inner_encountered == 2) {
					if (inner_token[0] == '0') {
						dcused = 0;
					} else if (inner_token[0] == '1') {
						dcused = 1;
					} else if (inner_token[0] == '2') {
						dcused = 2;
					} else {
						dbgio_printf("invalid mapping file, using defaults\n");
						goto map_parse_error;
					}
				} else if (inner_encountered == 3 && dcused > 0) {
					strcpy(dcbuts[0],inner_token);
				} else if (inner_encountered == 4 && dcused == 2) {
					strcpy(dcbuts[1],inner_token);
				} else {
					dbgio_printf("invalid mapping file, using defaults\n");
					goto map_parse_error;
				}

				inner_token = strtok_r(NULL, inner_delimiters, &inner_saveptr);
			}

			if (strncmp("INVALID", n64_control, 7)) {
				if (lineno == 1 && !strncmp("RIGHT", n64_control, 5)) {
					update_map(dcused, dcbuts, &ingame_mapping.map_right);
				} else if (lineno == 2 && !strncmp("LEFT", n64_control, 4)) {
					update_map(dcused, dcbuts, &ingame_mapping.map_left);
				} else if (lineno == 3 && !strncmp("UP", n64_control, 2)) {
					update_map(dcused, dcbuts, &ingame_mapping.map_up);
				} else if (lineno == 4 && !strncmp("DOWN", n64_control, 4)) {
					update_map(dcused, dcbuts, &ingame_mapping.map_down);
				} else if (lineno == 5 && !strncmp("ATTACK", n64_control, 6)) {
					update_map(dcused, dcbuts, &ingame_mapping.map_attack);
				} else if (lineno == 6 && !strncmp("USE", n64_control, 3)) {
					update_map(dcused, dcbuts, &ingame_mapping.map_use);
				} else if (lineno == 7 && !strncmp("AUTOMAP", n64_control, 7)) {
					update_map(dcused, dcbuts, &ingame_mapping.map_automap);
				} else if (lineno == 8 && !strncmp("SPEED", n64_control, 5)) {
					update_map(dcused, dcbuts, &ingame_mapping.map_speedonoff);
				} else if (lineno == 10 && !strncmp("STRAFELEFT", n64_control, 10)) {
					update_map(dcused, dcbuts, &ingame_mapping.map_strafeleft);
				} else if (lineno == 11 && !strncmp("STRAFERIGHT", n64_control, 11)) {
					update_map(dcused, dcbuts, &ingame_mapping.map_straferight);
				} else if (lineno == 9 && !strncmp("STRAFE", n64_control, 6)) {
					update_map(dcused, dcbuts, &ingame_mapping.map_strafeonoff);
				} else if (lineno == 12 && !strncmp("WEAPONBACKWARD", n64_control, 14)) {
					update_map(dcused, dcbuts, &ingame_mapping.map_weaponbackward);
				} else if (lineno == 13 && !strncmp("WEAPONFORWARD", n64_control, 13)) {
					update_map(dcused, dcbuts, &ingame_mapping.map_weaponforward);
				} else {
					dbgio_printf("invalid mapping file, using defaults\n");
					goto map_parse_error;
				}
			}

			token = strtok_r(NULL, outer_delimiters, &outer_saveptr);
		}

		if (lineno != 13) {
			dbgio_printf("invalid mapping file, using defaults\n");
			goto map_parse_error;
		}
	}	else {
map_parse_error:
		//dbgio_printf("Could not read mapping file, using default\n");
		memcpy(&ingame_mapping, &default_mapping, sizeof(mapped_buttons_t));
	}
}
