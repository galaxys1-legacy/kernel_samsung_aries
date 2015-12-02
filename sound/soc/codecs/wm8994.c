/*
 * wm8994.c  --  WM8994 ALSA Soc Audio driver
 *
 * Copyright 2010 Wolfson Microelectronics PLC.
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 * Notes:
 *  The WM8994 is a multichannel codec with S/PDIF support, featuring six
 *  DAC channels and two ADC channels.
 *
 *  Currently only the primary audio interface is supported - S/PDIF and
 *  the secondary audio interfaces are not.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/30pin_con.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/tlv.h>
#include <sound/initval.h>
#include <asm/div64.h>
#include <asm/io.h>
#include <plat/map-base.h>
#include <mach/regs-clock.h>
#include "wm8994.h"

#ifdef CONFIG_SND_WM8994_EXTENSIONS
#include "wm8994_extensions.h"
#endif

#define WM8994_VERSION "0.1"
#define SUBJECT "wm8994.c"

#if defined(CONFIG_VIDEO_TV20) && defined(CONFIG_SND_S5P_WM8994_MASTER)
#define HDMI_USE_AUDIO
#endif

/*
 *Definitions of clock related.
*/

static struct {
	int ratio;
	int clk_sys_rate;
} clk_sys_rates[] = {
	{ 64,   0 },
	{ 128,  1 },
	{ 192,  2 },
	{ 256,  3 },
	{ 384,  4 },
	{ 512,  5 },
	{ 768,  6 },
	{ 1024, 7 },
	{ 1408, 8 },
	{ 1536, 9 },
};

static struct {
	int rate;
	int sample_rate;
} sample_rates[] = {
	{ 8000,  0  },
	{ 11025, 1  },
	{ 12000, 2  },
	{ 16000, 3  },
	{ 22050, 4  },
	{ 24000, 5  },
	{ 32000, 6  },
	{ 44100, 7  },
	{ 48000, 8  },
	{ 88200, 9  },
	{ 96000, 10  },
};

static struct {
	int div;
	int bclk_div;
} bclk_divs[] = {
	{ 1,   0  },
	{ 2,   1  },
	{ 4,   2  },
	{ 6,   3  },
	{ 8,   4  },
	{ 12,  5  },
	{ 16,  6  },
	{ 24,  7  },
	{ 32,  8  },
	{ 48,  9  },
};

/*
 * Definitions of sound path
 */
select_route universal_wm8994_playback_paths[] = {
	wm8994_set_off, wm8994_set_playback_receiver,
	wm8994_set_playback_speaker, wm8994_set_playback_headset,
	wm8994_set_playback_headset, wm8994_set_playback_bluetooth,
	wm8994_set_playback_speaker_headset, wm8994_set_playback_extra_dock_speaker,
	wm8994_set_playback_hdmi_tvout,
	wm8994_set_playback_speaker_hdmitvout,
	wm8994_set_playback_speakerheadset_hdmitvout
};

select_route universal_wm8994_voicecall_paths[] = {
	wm8994_set_off, wm8994_set_voicecall_receiver,
	wm8994_set_voicecall_speaker, wm8994_set_voicecall_headset,
	wm8994_set_voicecall_headphone, wm8994_set_voicecall_bluetooth,
};

select_mic_route universal_wm8994_mic_paths[] = {
	wm8994_record_main_mic,
	wm8994_record_headset_mic,
	wm8994_record_bluetooth,
};


/*
 * Implementation of I2C functions
 */
static unsigned int wm8994_read_hw(struct snd_soc_codec *codec, u16 reg)
{
	struct i2c_msg xfer[2];
	u16 data;
	int ret;
	struct i2c_client *i2c = codec->control_data;

	data = ((reg & 0xff00) >> 8) | ((reg & 0xff) << 8);

	xfer[0].addr = i2c->addr;
	xfer[0].flags = 0;
	xfer[0].len = 2;
	xfer[0].buf = (void *)&data;

	xfer[1].addr = i2c->addr;
	xfer[1].flags = I2C_M_RD;
	xfer[1].len = 2;
	xfer[1].buf = (u8 *)&data;
	ret = i2c_transfer(i2c->adapter, xfer, 2);
	if (ret != 2) {
		dev_err(codec->dev, "Failed to read 0x%x: %d\n", reg, ret);
		return 0;
	}

	return (data >> 8) | ((data & 0xff) << 8);
}

int wm8994_write(struct snd_soc_codec *codec, unsigned int reg,
		 unsigned int value)
{
	u8 data[4];
	int ret;

#ifdef CONFIG_SND_WM8994_EXTENSIONS
	value = wm8994_extensions_write(codec, reg, value);
#endif

	/* data is
	 * D15..D9 WM8993 register offset
	 * D8...D0 register data
	 */
	data[0] = (reg & 0xff00) >> 8;
	data[1] = reg & 0x00ff;
	data[2] = value >> 8;
	data[3] = value & 0x00ff;
	ret = codec->hw_write(codec->control_data, data, 4);

	if (ret == 4)
		return 0;
	else {
		pr_err("i2c write problem occured\n");
		return ret;
	}
}

inline unsigned int wm8994_read(struct snd_soc_codec *codec, unsigned int reg)
{
	return wm8994_read_hw(codec, reg);
}

/*
 * Functions related volume.
 */
static const DECLARE_TLV_DB_SCALE(dac_tlv, -12750, 50, 1);

static int wm899x_outpga_put_volsw_vu(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *ucontrol)
{
	int ret;
	u16 val;

	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct soc_mixer_control *mc =
	    (struct soc_mixer_control *)kcontrol->private_value;
	int reg = mc->reg;

	DEBUG_LOG("");

	ret = snd_soc_put_volsw_2r(kcontrol, ucontrol);
	if (ret < 0)
		return ret;

	val = wm8994_read(codec, reg);

	return wm8994_write(codec, reg, val | 0x0100);
}

static int wm899x_inpga_put_volsw_vu(struct snd_kcontrol *kcontrol,
				     struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct soc_mixer_control *mc =
	    (struct soc_mixer_control *)kcontrol->private_value;
	int reg = mc->reg;
	int ret;
	u16 val;

	ret = snd_soc_put_volsw(kcontrol, ucontrol);

	if (ret < 0)
		return ret;

	val = wm8994_read(codec, reg);

	return wm8994_write(codec, reg, val | 0x0100);

}

/*
 * Implementation of sound path
 */
static const char *playback_path[] = { "OFF", "RCV", "SPK", "HP", "HP_NO_MIC",  "BT", "SPK_HP", "RING_SPK", "RING_HP", "RING_NO_MIC", "RING_SPK_HP", "EXTRA_DOCK_SPEAKER", "TV_OUT", "HDMI_TV_OUT", "HDMI_SPK", "HDMI_DUAL" };
static const char *voicecall_path[] = { "OFF", "RCV", "SPK", "HP", "HP_NO_MIC",  "BT", };
static const char *mic_path[] = {"Main Mic", "Hands Free Mic", "BT Sco Mic", "MIC OFF" };
static const char *fmradio_path[]   = { "FMR_OFF", "FMR_SPK", "FMR_HP", "FMR_SPK_MIX", "FMR_HP_MIX", "FMR_DUAL_MIX"};
static const char *codec_tuning_control[] = {"OFF", "ON"};
static const char *codec_status_control[] = {"FMR_VOL_0", "FMR_VOL_1", "FMR_OFF", "REC_OFF", "REC_ON"};
static const char *voice_record_path[] = {"CALL_RECORDING_OFF", "CALL_RECORDING_MAIN", "CALL_RECORDING_SUB"};
static const char *call_recording_channel[] ={"CH_OFF"," CH_UPLINK","CH_DOWNLINK","CH_UDLINK"};


<<<<<<< HEAD

static int wm8994_get_mic_path(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
=======
	wm8994->aifclk[aif] = rate;
>>>>>>> v3.1

	ucontrol->value.integer.value[0] = wm8994->rec_path;

	return 0;
}

static int wm8994_set_mic_path(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);

	DEBUG_LOG("");

	wm8994->codec_state |= CAPTURE_ACTIVE;

	switch (ucontrol->value.integer.value[0]) {
	case 0:
		wm8994->rec_path = MAIN;
		break;
	case 1:
		wm8994->rec_path = SUB;
		break;
	case 2:
		wm8994->rec_path = BT_REC;
		wm8994->universal_mic_path[wm8994->rec_path](codec);
		return 0;
	default:
		return -EINVAL;
	}

	if (wm8994->rec_path == MAIN)
		audio_ctrl_mic_bias_gpio(1);
	else { //SUB
		audio_ctrl_mic_bias_gpio(0);
		audio_ctrl_earmic_bias_gpio(1);
	}

	wm8994->universal_mic_path[wm8994->rec_path] (codec);

	return 0;
}

static int wm8994_get_path(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
/*
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);

	ucontrol->value.integer.value[0] = wm8994->cur_path;
*/
	return 0;
}

static int wm8994_set_path(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
	struct soc_enum *mc = (struct soc_enum *)kcontrol->private_value;
	int val;
	int path_num = ucontrol->value.integer.value[0];

	if (enable_audio_usb) {
		DEBUG_LOG("Enable audio USB\n");
		path_num = 11;
	}

	switch (path_num) {
	case PLAYBACK_OFF:
		DEBUG_LOG("Switching off output path\n");
		break;
	case RCV:
	case SPK:
	case HP:
	case HP_NO_MIC:
	case BT:
	case SPK_HP:
		DEBUG_LOG("routing to %s\n", mc->texts[path_num]);
		wm8994->ringtone_active = OFF;
		break;
	case RING_SPK:
	case RING_HP:
	case RING_NO_MIC:
		DEBUG_LOG("routing to %s\n", mc->texts[path_num]);
		wm8994->ringtone_active = ON;
		path_num -= 5;
		break;
	case RING_SPK_HP:
		DEBUG_LOG("routing to %s\n", mc->texts[path_num]);
		wm8994->ringtone_active = ON;
		path_num -= 4;
		break;
	case EXTRA_DOCK_SPEAKER:
		DEBUG_LOG("routing to %s\n", mc->texts[path_num]);
		wm8994->ringtone_active = OFF;
		path_num -= 4;
		break;
	case TV_OUT:
		DEBUG_LOG("routing to %s\n", mc->texts[path_num]);
		wm8994->ringtone_active = OFF;
		path_num -= 4;
		break;
	case HDMI_TV_OUT:
	case HDMI_SPK:
	case HDMI_DUAL:
		DEBUG_LOG("routing to %s\n", mc->texts[path_num]);
		wm8994->ringtone_active = OFF;
		path_num -= 5;
		break;
	default:
		DEBUG_LOG_ERR("audio path[%d] does not exists!!\n", path_num);
		return -ENODEV;
		break;
	}

	wm8994->codec_state |= PLAYBACK_ACTIVE;

	if (wm8994->codec_state & FMRADIO_ACTIVE) {
		wm8994->codec_state &= ~(FMRADIO_ACTIVE);
		wm8994->fmr_mix_path = FMR_MIX_OFF;
		wm8994->fmradio_path = FMR_OFF;
	}

	if (wm8994->codec_state & CALL_ACTIVE) {
		wm8994->codec_state &= ~(CALL_ACTIVE);

		val = wm8994_read(codec, WM8994_CLOCKING_1);
		val &= ~(WM8994_DSP_FS2CLK_ENA_MASK | WM8994_SYSCLK_SRC_MASK);
		wm8994_write(codec, WM8994_CLOCKING_1, val);
	}


	if (wm8994->testmode_config_flag == SEC_TEST_PBA_DUAL_SPK &&
		(path_num == HP || path_num == HP_NO_MIC)) {
		DEBUG_LOG("SEC_TEST_PBA_DUAL_SPK : forced set path to SPK");
		path_num = SPK;
	}

	wm8994->cur_path = path_num;
	wm8994->universal_playback_path[wm8994->cur_path] (codec);

	return 0;
}

static int wm8994_get_call_path(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	return 0;
}

static int wm8994_set_call_path(struct snd_kcontrol *kcontrol,
        struct snd_ctl_elem_value *ucontrol)
{
    struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
	struct soc_enum *mc = (struct soc_enum *)kcontrol->private_value;

	// Get path value
	int path_num = ucontrol->value.integer.value[0];

	if (strcmp( mc->texts[path_num], voicecall_path[path_num]) ) {
		DEBUG_LOG_ERR("Unknown path %s", mc->texts[path_num] );
		return -ENODEV;
	}

	switch (path_num) {
		case PLAYBACK_OFF :
			DEBUG_LOG("Switching off output path");
			break;

		case SPK :
		case RCV :
		case HP:
		case HP_NO_MIC:
		case BT :
			DEBUG_LOG("routing  voice path to %s", mc->texts[path_num] );
			break;

		default:
			DEBUG_LOG_ERR("The audio path[%d] does not exists!!", path_num);
			return -ENODEV;
			break;
	}

	if (wm8994->cur_path != path_num || !(wm8994->codec_state & CALL_ACTIVE)) {
		wm8994->codec_state |= CALL_ACTIVE;
		wm8994->cur_path = path_num;
		wm8994->universal_voicecall_path[wm8994->cur_path](codec);
	} else {
		int val;

		val = wm8994_read(codec, WM8994_AIF1_DAC1_FILTERS_1);
		val &= ~(WM8994_AIF1DAC1_MUTE_MASK);
		val |= (WM8994_AIF1DAC1_UNMUTE);
		wm8994_write(codec, WM8994_AIF1_DAC1_FILTERS_1, val);
	}

	return 0;
}


static int wm8994_get_fmradio_path(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	return 0;
}

static int wm8994_set_fmradio_path(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct soc_enum *mc = (struct soc_enum *)kcontrol->private_value;
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);

	int path_num = ucontrol->value.integer.value[0];

	if (strcmp(mc->texts[path_num], fmradio_path[path_num]))
		DEBUG_LOG("Unknown path %s\n", mc->texts[path_num]);

	if (path_num == wm8994->fmradio_path) {
		int val;

		DEBUG_LOG("%s is already set. skip to set path..\n",
			mc->texts[path_num]);

		val = wm8994_read(codec, WM8994_AIF1_DAC1_FILTERS_1);
		val &= ~(WM8994_AIF1DAC1_MUTE_MASK);
		val |= (WM8994_AIF1DAC1_UNMUTE);
		wm8994_write(codec, WM8994_AIF1_DAC1_FILTERS_1, val);

		return 0;
	}

	wm8994->codec_state |= FMRADIO_ACTIVE;

	switch (path_num) {
	case FMR_OFF:
		DEBUG_LOG("Switching off output path");
		wm8994_disable_fmradio_path(codec, FMR_OFF);
		break;
	case FMR_SPK:
		DEBUG_LOG("routing  fmradio path to %s", mc->texts[path_num] );
		wm8994->fmr_mix_path = FMR_MIX_OFF;
		wm8994_set_fmradio_speaker(codec);
		break;
	case FMR_HP:
		DEBUG_LOG("routing fmradio path to %s\n", mc->texts[path_num]);
		wm8994->fmr_mix_path = FMR_MIX_OFF;
		wm8994_set_fmradio_headset(codec);
		break;

	case FMR_SPK_MIX:
		DEBUG_LOG("routing  fmradio path to %s", mc->texts[path_num]);

		if (wm8994->fmr_mix_path != FMR_MIX_SPK) {
			wm8994->fmr_mix_path = FMR_MIX_SPK;
			wm8994_set_fmradio_speaker_mix(codec);
		} else {
			wm8994_write(codec,WM8994_AIF1_DAC1_FILTERS_1, WM8994_AIF1DAC1_UNMUTE);
			DEBUG_LOG("FMR_MIX_SPK is already set!!! Skip path!!");
		}
		break;

	case FMR_HP_MIX:
		DEBUG_LOG("routing  fmradio path to %s", mc->texts[path_num]);

		if (wm8994->fmr_mix_path != FMR_MIX_HP) {
			wm8994->fmr_mix_path = FMR_MIX_HP;
			wm8994_set_fmradio_headset_mix(codec);
		} else {
			wm8994_write(codec,WM8994_AIF1_DAC1_FILTERS_1, WM8994_AIF1DAC1_UNMUTE);
			DEBUG_LOG("FMR_MIX_HP is already set!!! Skip path!!");
		}
		break;

	case FMR_DUAL_MIX :
		DEBUG_LOG("routing  fmradio path to  %s\n", mc->texts[path_num]);
		wm8994->fmr_mix_path = FMR_DUAL_MIX;
		wm8994_set_fmradio_speaker_headset_mix(codec);
		break;

	default:
		DEBUG_LOG("The audio path[%d] does not exists!!\n",path_num);
		return -ENODEV;
		break;
	}

	return 0;
}

static int wm8994_get_codec_tuning(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);

	DEBUG_LOG("Get testmode_config_flag = [%d]", wm8994->testmode_config_flag);
	ucontrol->value.integer.value[0] = wm8994->testmode_config_flag;

	return 0;
}

static int wm8994_set_codec_tuning(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);

	int control_flag = ucontrol->value.integer.value[0];

	DEBUG_LOG("Set testmode_config_flag =[%d]", control_flag);

	wm8994->testmode_config_flag = control_flag;

	return 0;
}

static int wm8994_get_codec_status(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	return 0;
}

static void wm8994_shutdown_codec(struct snd_pcm_substream *substream, struct snd_soc_codec *codec);

static int wm8994_set_codec_status(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
	struct snd_pcm_substream tempstream;

	int control_data = ucontrol->value.integer.value[0];

	DEBUG_LOG("Received control_data = [0x%X]", control_data);

	switch (control_data) {
	/* FM Radio Volume zero control */
	case CMD_FMR_INPUT_DEACTIVE:
	case CMD_FMR_INPUT_ACTIVE:
		if (wm8994->codec_state & FMRADIO_ACTIVE)
			wm8994_set_fmradio_common(codec, control_data);
		break;

	/* To remove pop up noise for FM radio */
	case CMD_FMR_FLAG_CLEAR:
		DEBUG_LOG("FM Radio Flag is clear!!");
		wm8994->codec_state &= ~(FMRADIO_ACTIVE);
		break;

	case CMD_FMR_END:
		DEBUG_LOG("Call shutdown function forcely for FM radio!!");
		wm8994->codec_state &= ~(FMRADIO_ACTIVE);
		tempstream.stream = SNDRV_PCM_STREAM_PLAYBACK;
		wm8994_shutdown_codec(&tempstream, codec);
		break;

	/* For voice recognition. */
	case CMD_RECOGNITION_DEACTIVE :
		DEBUG_LOG("Recognition Gain is deactivated!!");
		wm8994->recognition_active = REC_OFF;
		break;

	case CMD_RECOGNITION_ACTIVE :
		DEBUG_LOG("Recognition Gain is activated!!");
		wm8994->recognition_active = REC_ON;
		break;

	/* To remove pop up noise for Call. */
	case CMD_CALL_FLAG_CLEAR:
		DEBUG_LOG("Call Flag is clear!!");
		wm8994->codec_state &= ~(CALL_ACTIVE);
		break;

	case CMD_CALL_END:
		DEBUG_LOG("Call shutdown function forcely for call!!");
		wm8994->codec_state &= ~(CALL_ACTIVE);
		tempstream.stream = SNDRV_PCM_STREAM_PLAYBACK;
		wm8994_shutdown_codec(&tempstream, codec);
		break;

#ifdef FEATURE_VSUITE_RECOGNITION
	// For vsuite voice recognition.
	case CMD_VSUITE_RECOGNITION_DEACTIVE :
		DEBUG_LOG("VSuite recognition Gain is deactivated!!");
		wm8994->vsuite_recognition_active = REC_OFF;
		break;

	case CMD_VSUITE_RECOGNITION_ACTIVE :
		DEBUG_LOG("VSuite recognition Gain is activated!!");
		wm8994->vsuite_recognition_active = REC_ON;
		break;
#endif

	default:
		break;
	}

	return 0;
}

static int wm8994_get_voice_recording_ch(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	return 0;
}

static int wm8994_set_voice_recording_ch(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);


	int channel = ucontrol->value.integer.value[0];

	wm8994 ->call_record_ch = channel;

	DEBUG_LOG("control_data = [0x%X]", channel);


	switch (channel) {
	case CH_OFF:
		wm8994 ->call_record_path = CALL_RECORDING_OFF;
		wm8994_set_voicecall_record_off(codec);
		break;
	case CH_UPLINK:
	case CH_DOWNLINK:
	case CH_UDLINK:
		break;

	default :
		break;
	}

	return 0;
}

static int wm8994_get_voice_call_recording(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	return 0;
}

static int wm8994_set_voice_call_recording(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);

	int path_num = ucontrol->value.integer.value[0];

	DEBUG_LOG("control_data = [0x%X]", path_num);

	wm8994 ->call_record_path = path_num;

	switch (path_num) {
	case CALL_RECORDING_OFF :
		break;
	case CALL_RECORDING_MAIN :
	case CALL_RECORDING_SUB :
		wm8994_set_voicecall_record(codec, (int)wm8994 ->call_record_ch);
		break;
	default :
		break;
	}

	return 0;
}

void wm8994_set_off(struct snd_soc_codec *codec)
{
	DEBUG_LOG("");
	audio_power(0);
}

#define  SOC_WM899X_OUTPGA_DOUBLE_R_TLV(xname, reg_left, reg_right,\
		xshift, xmax, xinvert, tlv_array) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = (xname),\
	.access = SNDRV_CTL_ELEM_ACCESS_TLV_READ |\
		 SNDRV_CTL_ELEM_ACCESS_READWRITE,\
	.tlv.p = (tlv_array), \
	.info = snd_soc_info_volsw_2r, \
	.get = snd_soc_get_volsw_2r, .put = wm899x_outpga_put_volsw_vu, \
	.private_value = (unsigned long)&(struct soc_mixer_control) \
		{.reg = reg_left, .rreg = reg_right, .shift = xshift, \
		.max = xmax, .invert = xinvert} }

#define SOC_WM899X_OUTPGA_SINGLE_R_TLV(xname, reg, shift, max, invert,\
		tlv_array) {\
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = (xname), \
		.access = SNDRV_CTL_ELEM_ACCESS_TLV_READ |\
				SNDRV_CTL_ELEM_ACCESS_READWRITE,\
		.tlv.p = (tlv_array), \
		.info = snd_soc_info_volsw, \
		.get = snd_soc_get_volsw, .put = wm899x_inpga_put_volsw_vu, \
		.private_value = SOC_SINGLE_VALUE(reg, shift, max, invert) }

static const DECLARE_TLV_DB_SCALE(digital_tlv, -7162, 37, 1);
static const DECLARE_TLV_DB_LINEAR(digital_tlv_spkr, -5700, 600);
static const DECLARE_TLV_DB_LINEAR(digital_tlv_rcv, -5700, 600);
static const DECLARE_TLV_DB_LINEAR(digital_tlv_headphone, -5700, 600);
static const DECLARE_TLV_DB_LINEAR(digital_tlv_mic, -7162, 7162);

static const struct soc_enum path_control_enum[] = {
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(playback_path), playback_path),
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(voicecall_path), voicecall_path),
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(mic_path), mic_path),
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(fmradio_path),fmradio_path),
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(codec_tuning_control), codec_tuning_control),
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(codec_status_control), codec_status_control),
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(voice_record_path), voice_record_path),
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(call_recording_channel), call_recording_channel),
};

static const struct snd_kcontrol_new wm8994_snd_controls[] = {
	SOC_WM899X_OUTPGA_DOUBLE_R_TLV("Playback Volume",
				       WM8994_LEFT_OPGA_VOLUME,
				       WM8994_RIGHT_OPGA_VOLUME, 0, 0x3F, 0,
				       digital_tlv_rcv),
	SOC_WM899X_OUTPGA_DOUBLE_R_TLV("Playback Spkr Volume",
				       WM8994_SPEAKER_VOLUME_LEFT,
				       WM8994_SPEAKER_VOLUME_RIGHT, 1, 0x3F, 0,
				       digital_tlv_spkr),
	SOC_WM899X_OUTPGA_DOUBLE_R_TLV("Playback Headset Volume",
				       WM8994_LEFT_OUTPUT_VOLUME,
				       WM8994_RIGHT_OUTPUT_VOLUME, 1, 0x3F, 0,
				       digital_tlv_headphone),
	SOC_WM899X_OUTPGA_SINGLE_R_TLV("Capture Volume",
				       WM8994_AIF1_ADC1_LEFT_VOLUME,
				       0, 0xEF, 0, digital_tlv_mic),
	/* Path Control */
	SOC_ENUM_EXT("Playback Path", path_control_enum[0],
		     wm8994_get_path, wm8994_set_path),

	SOC_ENUM_EXT("Voice Call Path", path_control_enum[1],
		     wm8994_get_call_path, wm8994_set_call_path),

	SOC_ENUM_EXT("Capture MIC Path", path_control_enum[2],
		     wm8994_get_mic_path, wm8994_set_mic_path),

	SOC_ENUM_EXT("FM Radio Path", path_control_enum[3],
		     wm8994_get_fmradio_path, wm8994_set_fmradio_path),

	SOC_ENUM_EXT("Codec Tuning", path_control_enum[4],
			wm8994_get_codec_tuning, wm8994_set_codec_tuning),

	SOC_ENUM_EXT("Codec Status", path_control_enum[5],
			wm8994_get_codec_status, wm8994_set_codec_status),

	SOC_ENUM_EXT("Voice Call Recording", path_control_enum[6],
			wm8994_get_voice_call_recording, wm8994_set_voice_call_recording),

	SOC_ENUM_EXT("Recording Channel", path_control_enum[7],
			wm8994_get_voice_recording_ch, wm8994_set_voice_recording_ch),

};

/* Add non-DAPM controls */
static int wm8994_add_controls(struct snd_soc_codec *codec)
{
	return snd_soc_add_controls(codec, wm8994_snd_controls,
				    ARRAY_SIZE(wm8994_snd_controls));
}
<<<<<<< HEAD
static const struct snd_soc_dapm_widget wm8994_dapm_widgets[] = {
=======

static const struct snd_kcontrol_new dac1l_mix[] = {
WM8994_CLASS_W_SWITCH("Right Sidetone Switch", WM8994_DAC1_LEFT_MIXER_ROUTING,
		      5, 1, 0),
WM8994_CLASS_W_SWITCH("Left Sidetone Switch", WM8994_DAC1_LEFT_MIXER_ROUTING,
		      4, 1, 0),
WM8994_CLASS_W_SWITCH("AIF2 Switch", WM8994_DAC1_LEFT_MIXER_ROUTING,
		      2, 1, 0),
WM8994_CLASS_W_SWITCH("AIF1.2 Switch", WM8994_DAC1_LEFT_MIXER_ROUTING,
		      1, 1, 0),
WM8994_CLASS_W_SWITCH("AIF1.1 Switch", WM8994_DAC1_LEFT_MIXER_ROUTING,
		      0, 1, 0),
};

static const struct snd_kcontrol_new dac1r_mix[] = {
WM8994_CLASS_W_SWITCH("Right Sidetone Switch", WM8994_DAC1_RIGHT_MIXER_ROUTING,
		      5, 1, 0),
WM8994_CLASS_W_SWITCH("Left Sidetone Switch", WM8994_DAC1_RIGHT_MIXER_ROUTING,
		      4, 1, 0),
WM8994_CLASS_W_SWITCH("AIF2 Switch", WM8994_DAC1_RIGHT_MIXER_ROUTING,
		      2, 1, 0),
WM8994_CLASS_W_SWITCH("AIF1.2 Switch", WM8994_DAC1_RIGHT_MIXER_ROUTING,
		      1, 1, 0),
WM8994_CLASS_W_SWITCH("AIF1.1 Switch", WM8994_DAC1_RIGHT_MIXER_ROUTING,
		      0, 1, 0),
};

static const char *sidetone_text[] = {
	"ADC/DMIC1", "DMIC2",
};

static const struct soc_enum sidetone1_enum =
	SOC_ENUM_SINGLE(WM8994_SIDETONE, 0, 2, sidetone_text);

static const struct snd_kcontrol_new sidetone1_mux =
	SOC_DAPM_ENUM("Left Sidetone Mux", sidetone1_enum);

static const struct soc_enum sidetone2_enum =
	SOC_ENUM_SINGLE(WM8994_SIDETONE, 1, 2, sidetone_text);

static const struct snd_kcontrol_new sidetone2_mux =
	SOC_DAPM_ENUM("Right Sidetone Mux", sidetone2_enum);

static const char *aif1dac_text[] = {
	"AIF1DACDAT", "AIF3DACDAT",
};

static const struct soc_enum aif1dac_enum =
	SOC_ENUM_SINGLE(WM8994_POWER_MANAGEMENT_6, 0, 2, aif1dac_text);

static const struct snd_kcontrol_new aif1dac_mux =
	SOC_DAPM_ENUM("AIF1DAC Mux", aif1dac_enum);

static const char *aif2dac_text[] = {
	"AIF2DACDAT", "AIF3DACDAT",
};

static const struct soc_enum aif2dac_enum =
	SOC_ENUM_SINGLE(WM8994_POWER_MANAGEMENT_6, 1, 2, aif2dac_text);

static const struct snd_kcontrol_new aif2dac_mux =
	SOC_DAPM_ENUM("AIF2DAC Mux", aif2dac_enum);

static const char *aif2adc_text[] = {
	"AIF2ADCDAT", "AIF3DACDAT",
};

static const struct soc_enum aif2adc_enum =
	SOC_ENUM_SINGLE(WM8994_POWER_MANAGEMENT_6, 2, 2, aif2adc_text);

static const struct snd_kcontrol_new aif2adc_mux =
	SOC_DAPM_ENUM("AIF2ADC Mux", aif2adc_enum);

static const char *aif3adc_text[] = {
	"AIF1ADCDAT", "AIF2ADCDAT", "AIF2DACDAT", "Mono PCM",
};

static const struct soc_enum wm8994_aif3adc_enum =
	SOC_ENUM_SINGLE(WM8994_POWER_MANAGEMENT_6, 3, 3, aif3adc_text);

static const struct snd_kcontrol_new wm8994_aif3adc_mux =
	SOC_DAPM_ENUM("AIF3ADC Mux", wm8994_aif3adc_enum);

static const struct soc_enum wm8958_aif3adc_enum =
	SOC_ENUM_SINGLE(WM8994_POWER_MANAGEMENT_6, 3, 4, aif3adc_text);

static const struct snd_kcontrol_new wm8958_aif3adc_mux =
	SOC_DAPM_ENUM("AIF3ADC Mux", wm8958_aif3adc_enum);

static const char *mono_pcm_out_text[] = {
	"None", "AIF2ADCL", "AIF2ADCR", 
};

static const struct soc_enum mono_pcm_out_enum =
	SOC_ENUM_SINGLE(WM8994_POWER_MANAGEMENT_6, 9, 3, mono_pcm_out_text);

static const struct snd_kcontrol_new mono_pcm_out_mux =
	SOC_DAPM_ENUM("Mono PCM Out Mux", mono_pcm_out_enum);

static const char *aif2dac_src_text[] = {
	"AIF2", "AIF3",
};

/* Note that these two control shouldn't be simultaneously switched to AIF3 */
static const struct soc_enum aif2dacl_src_enum =
	SOC_ENUM_SINGLE(WM8994_POWER_MANAGEMENT_6, 7, 2, aif2dac_src_text);

static const struct snd_kcontrol_new aif2dacl_src_mux =
	SOC_DAPM_ENUM("AIF2DACL Mux", aif2dacl_src_enum);

static const struct soc_enum aif2dacr_src_enum =
	SOC_ENUM_SINGLE(WM8994_POWER_MANAGEMENT_6, 8, 2, aif2dac_src_text);

static const struct snd_kcontrol_new aif2dacr_src_mux =
	SOC_DAPM_ENUM("AIF2DACR Mux", aif2dacr_src_enum);

static const struct snd_soc_dapm_widget wm8994_lateclk_revd_widgets[] = {
SND_SOC_DAPM_SUPPLY("AIF1CLK", SND_SOC_NOPM, 0, 0, aif1clk_ev,
	SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
SND_SOC_DAPM_SUPPLY("AIF2CLK", SND_SOC_NOPM, 0, 0, aif2clk_ev,
	SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),

SND_SOC_DAPM_PGA_E("Late DAC1L Enable PGA", SND_SOC_NOPM, 0, 0, NULL, 0,
	late_enable_ev, SND_SOC_DAPM_PRE_PMU),
SND_SOC_DAPM_PGA_E("Late DAC1R Enable PGA", SND_SOC_NOPM, 0, 0, NULL, 0,
	late_enable_ev, SND_SOC_DAPM_PRE_PMU),
SND_SOC_DAPM_PGA_E("Late DAC2L Enable PGA", SND_SOC_NOPM, 0, 0, NULL, 0,
	late_enable_ev, SND_SOC_DAPM_PRE_PMU),
SND_SOC_DAPM_PGA_E("Late DAC2R Enable PGA", SND_SOC_NOPM, 0, 0, NULL, 0,
	late_enable_ev, SND_SOC_DAPM_PRE_PMU),
SND_SOC_DAPM_PGA_E("Direct Voice", SND_SOC_NOPM, 0, 0, NULL, 0,
	late_enable_ev, SND_SOC_DAPM_PRE_PMU),

SND_SOC_DAPM_MIXER_E("SPKL", WM8994_POWER_MANAGEMENT_3, 8, 0,
		     left_speaker_mixer, ARRAY_SIZE(left_speaker_mixer),
		     late_enable_ev, SND_SOC_DAPM_PRE_PMU),
SND_SOC_DAPM_MIXER_E("SPKR", WM8994_POWER_MANAGEMENT_3, 9, 0,
		     right_speaker_mixer, ARRAY_SIZE(right_speaker_mixer),
		     late_enable_ev, SND_SOC_DAPM_PRE_PMU),
SND_SOC_DAPM_MUX_E("Left Headphone Mux", SND_SOC_NOPM, 0, 0, &hpl_mux,
		   late_enable_ev, SND_SOC_DAPM_PRE_PMU),
SND_SOC_DAPM_MUX_E("Right Headphone Mux", SND_SOC_NOPM, 0, 0, &hpr_mux,
		   late_enable_ev, SND_SOC_DAPM_PRE_PMU),

SND_SOC_DAPM_POST("Late Disable PGA", late_disable_ev)
};

static const struct snd_soc_dapm_widget wm8994_lateclk_widgets[] = {
SND_SOC_DAPM_SUPPLY("AIF1CLK", WM8994_AIF1_CLOCKING_1, 0, 0, NULL, 0),
SND_SOC_DAPM_SUPPLY("AIF2CLK", WM8994_AIF2_CLOCKING_1, 0, 0, NULL, 0),
SND_SOC_DAPM_PGA("Direct Voice", SND_SOC_NOPM, 0, 0, NULL, 0),
SND_SOC_DAPM_MIXER("SPKL", WM8994_POWER_MANAGEMENT_3, 8, 0,
		   left_speaker_mixer, ARRAY_SIZE(left_speaker_mixer)),
SND_SOC_DAPM_MIXER("SPKR", WM8994_POWER_MANAGEMENT_3, 9, 0,
		   right_speaker_mixer, ARRAY_SIZE(right_speaker_mixer)),
SND_SOC_DAPM_MUX("Left Headphone Mux", SND_SOC_NOPM, 0, 0, &hpl_mux),
SND_SOC_DAPM_MUX("Right Headphone Mux", SND_SOC_NOPM, 0, 0, &hpr_mux),
};

static const struct snd_soc_dapm_widget wm8994_dac_revd_widgets[] = {
SND_SOC_DAPM_DAC_E("DAC2L", NULL, SND_SOC_NOPM, 3, 0,
	dac_ev, SND_SOC_DAPM_PRE_PMU),
SND_SOC_DAPM_DAC_E("DAC2R", NULL, SND_SOC_NOPM, 2, 0,
	dac_ev, SND_SOC_DAPM_PRE_PMU),
SND_SOC_DAPM_DAC_E("DAC1L", NULL, SND_SOC_NOPM, 1, 0,
	dac_ev, SND_SOC_DAPM_PRE_PMU),
SND_SOC_DAPM_DAC_E("DAC1R", NULL, SND_SOC_NOPM, 0, 0,
	dac_ev, SND_SOC_DAPM_PRE_PMU),
};

static const struct snd_soc_dapm_widget wm8994_dac_widgets[] = {
SND_SOC_DAPM_DAC("DAC2L", NULL, WM8994_POWER_MANAGEMENT_5, 3, 0),
SND_SOC_DAPM_DAC("DAC2R", NULL, WM8994_POWER_MANAGEMENT_5, 2, 0),
SND_SOC_DAPM_DAC("DAC1L", NULL, WM8994_POWER_MANAGEMENT_5, 1, 0),
SND_SOC_DAPM_DAC("DAC1R", NULL, WM8994_POWER_MANAGEMENT_5, 0, 0),
};

static const struct snd_soc_dapm_widget wm8994_adc_revd_widgets[] = {
SND_SOC_DAPM_MUX_E("ADCL Mux", WM8994_POWER_MANAGEMENT_4, 1, 0, &adcl_mux,
		   adc_mux_ev, SND_SOC_DAPM_PRE_PMU),
SND_SOC_DAPM_MUX_E("ADCR Mux", WM8994_POWER_MANAGEMENT_4, 0, 0, &adcr_mux,
		   adc_mux_ev, SND_SOC_DAPM_PRE_PMU),
};

static const struct snd_soc_dapm_widget wm8994_adc_widgets[] = {
SND_SOC_DAPM_MUX("ADCL Mux", WM8994_POWER_MANAGEMENT_4, 1, 0, &adcl_mux),
SND_SOC_DAPM_MUX("ADCR Mux", WM8994_POWER_MANAGEMENT_4, 0, 0, &adcr_mux),
};

static const struct snd_soc_dapm_widget wm8994_dapm_widgets[] = {
SND_SOC_DAPM_INPUT("DMIC1DAT"),
SND_SOC_DAPM_INPUT("DMIC2DAT"),
SND_SOC_DAPM_INPUT("Clock"),

SND_SOC_DAPM_SUPPLY_S("MICBIAS Supply", 1, SND_SOC_NOPM, 0, 0, micbias_ev,
		      SND_SOC_DAPM_PRE_PMU),

SND_SOC_DAPM_SUPPLY("CLK_SYS", SND_SOC_NOPM, 0, 0, clk_sys_event,
		    SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),

SND_SOC_DAPM_SUPPLY("DSP1CLK", WM8994_CLOCKING_1, 3, 0, NULL, 0),
SND_SOC_DAPM_SUPPLY("DSP2CLK", WM8994_CLOCKING_1, 2, 0, NULL, 0),
SND_SOC_DAPM_SUPPLY("DSPINTCLK", WM8994_CLOCKING_1, 1, 0, NULL, 0),

SND_SOC_DAPM_AIF_OUT("AIF1ADC1L", NULL,
		     0, WM8994_POWER_MANAGEMENT_4, 9, 0),
SND_SOC_DAPM_AIF_OUT("AIF1ADC1R", NULL,
		     0, WM8994_POWER_MANAGEMENT_4, 8, 0),
SND_SOC_DAPM_AIF_IN_E("AIF1DAC1L", NULL, 0,
		      WM8994_POWER_MANAGEMENT_5, 9, 0, wm8958_aif_ev,
		      SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
SND_SOC_DAPM_AIF_IN_E("AIF1DAC1R", NULL, 0,
		      WM8994_POWER_MANAGEMENT_5, 8, 0, wm8958_aif_ev,
		      SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),

SND_SOC_DAPM_AIF_OUT("AIF1ADC2L", NULL,
		     0, WM8994_POWER_MANAGEMENT_4, 11, 0),
SND_SOC_DAPM_AIF_OUT("AIF1ADC2R", NULL,
		     0, WM8994_POWER_MANAGEMENT_4, 10, 0),
SND_SOC_DAPM_AIF_IN_E("AIF1DAC2L", NULL, 0,
		      WM8994_POWER_MANAGEMENT_5, 11, 0, wm8958_aif_ev,
		      SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
SND_SOC_DAPM_AIF_IN_E("AIF1DAC2R", NULL, 0,
		      WM8994_POWER_MANAGEMENT_5, 10, 0, wm8958_aif_ev,
		      SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),

SND_SOC_DAPM_MIXER("AIF1ADC1L Mixer", SND_SOC_NOPM, 0, 0,
		   aif1adc1l_mix, ARRAY_SIZE(aif1adc1l_mix)),
SND_SOC_DAPM_MIXER("AIF1ADC1R Mixer", SND_SOC_NOPM, 0, 0,
		   aif1adc1r_mix, ARRAY_SIZE(aif1adc1r_mix)),

SND_SOC_DAPM_MIXER("AIF1ADC2L Mixer", SND_SOC_NOPM, 0, 0,
		   aif1adc2l_mix, ARRAY_SIZE(aif1adc2l_mix)),
SND_SOC_DAPM_MIXER("AIF1ADC2R Mixer", SND_SOC_NOPM, 0, 0,
		   aif1adc2r_mix, ARRAY_SIZE(aif1adc2r_mix)),

SND_SOC_DAPM_MIXER("AIF2DAC2L Mixer", SND_SOC_NOPM, 0, 0,
		   aif2dac2l_mix, ARRAY_SIZE(aif2dac2l_mix)),
SND_SOC_DAPM_MIXER("AIF2DAC2R Mixer", SND_SOC_NOPM, 0, 0,
		   aif2dac2r_mix, ARRAY_SIZE(aif2dac2r_mix)),

SND_SOC_DAPM_MUX("Left Sidetone", SND_SOC_NOPM, 0, 0, &sidetone1_mux),
SND_SOC_DAPM_MUX("Right Sidetone", SND_SOC_NOPM, 0, 0, &sidetone2_mux),

SND_SOC_DAPM_MIXER("DAC1L Mixer", SND_SOC_NOPM, 0, 0,
		   dac1l_mix, ARRAY_SIZE(dac1l_mix)),
SND_SOC_DAPM_MIXER("DAC1R Mixer", SND_SOC_NOPM, 0, 0,
		   dac1r_mix, ARRAY_SIZE(dac1r_mix)),

SND_SOC_DAPM_AIF_OUT("AIF2ADCL", NULL, 0,
		     WM8994_POWER_MANAGEMENT_4, 13, 0),
SND_SOC_DAPM_AIF_OUT("AIF2ADCR", NULL, 0,
		     WM8994_POWER_MANAGEMENT_4, 12, 0),
SND_SOC_DAPM_AIF_IN_E("AIF2DACL", NULL, 0,
		      WM8994_POWER_MANAGEMENT_5, 13, 0, wm8958_aif_ev,
		      SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
SND_SOC_DAPM_AIF_IN_E("AIF2DACR", NULL, 0,
		      WM8994_POWER_MANAGEMENT_5, 12, 0, wm8958_aif_ev,
		      SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),

SND_SOC_DAPM_AIF_IN("AIF1DACDAT", "AIF1 Playback", 0, SND_SOC_NOPM, 0, 0),
SND_SOC_DAPM_AIF_IN("AIF2DACDAT", "AIF2 Playback", 0, SND_SOC_NOPM, 0, 0),
SND_SOC_DAPM_AIF_OUT("AIF1ADCDAT", "AIF1 Capture", 0, SND_SOC_NOPM, 0, 0),
SND_SOC_DAPM_AIF_OUT("AIF2ADCDAT", "AIF2 Capture", 0, SND_SOC_NOPM, 0, 0),

SND_SOC_DAPM_MUX("AIF1DAC Mux", SND_SOC_NOPM, 0, 0, &aif1dac_mux),
SND_SOC_DAPM_MUX("AIF2DAC Mux", SND_SOC_NOPM, 0, 0, &aif2dac_mux),
SND_SOC_DAPM_MUX("AIF2ADC Mux", SND_SOC_NOPM, 0, 0, &aif2adc_mux),

SND_SOC_DAPM_AIF_IN("AIF3DACDAT", "AIF3 Playback", 0, SND_SOC_NOPM, 0, 0),
SND_SOC_DAPM_AIF_IN("AIF3ADCDAT", "AIF3 Capture", 0, SND_SOC_NOPM, 0, 0),

SND_SOC_DAPM_SUPPLY("TOCLK", WM8994_CLOCKING_1, 4, 0, NULL, 0),

SND_SOC_DAPM_ADC("DMIC2L", NULL, WM8994_POWER_MANAGEMENT_4, 5, 0),
SND_SOC_DAPM_ADC("DMIC2R", NULL, WM8994_POWER_MANAGEMENT_4, 4, 0),
SND_SOC_DAPM_ADC("DMIC1L", NULL, WM8994_POWER_MANAGEMENT_4, 3, 0),
SND_SOC_DAPM_ADC("DMIC1R", NULL, WM8994_POWER_MANAGEMENT_4, 2, 0),

/* Power is done with the muxes since the ADC power also controls the
 * downsampling chain, the chip will automatically manage the analogue
 * specific portions.
 */
SND_SOC_DAPM_ADC("ADCL", NULL, SND_SOC_NOPM, 1, 0),
SND_SOC_DAPM_ADC("ADCR", NULL, SND_SOC_NOPM, 0, 0),

SND_SOC_DAPM_POST("Debug log", post_ev),
>>>>>>> v3.1
};

static const struct snd_soc_dapm_route audio_map[] = {
};

static int wm8994_add_widgets(struct snd_soc_codec *codec)
{
	snd_soc_dapm_new_controls(&codec->dapm, wm8994_dapm_widgets,
			ARRAY_SIZE(wm8994_dapm_widgets));

	snd_soc_dapm_add_routes(&codec->dapm, audio_map, ARRAY_SIZE(audio_map));

	snd_soc_dapm_new_widgets(&codec->dapm);
	return 0;
}

static int configure_clock(struct snd_soc_codec *codec)
{
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
<<<<<<< HEAD
	unsigned int reg;
=======
	int reg_offset, ret;
	struct fll_div fll;
	u16 reg, aif1, aif2;
	unsigned long timeout;

	aif1 = snd_soc_read(codec, WM8994_AIF1_CLOCKING_1)
		& WM8994_AIF1CLK_ENA;
>>>>>>> v3.1

	DEBUG_LOG("");

	if (wm8994->codec_state != DEACTIVE) {
		DEBUG_LOG("Codec is already actvied. Skip clock setting.");
		return 0;
<<<<<<< HEAD
=======

	/* If we're stopping the FLL redo the old config - no
	 * registers will actually be written but we avoid GCC flow
	 * analysis bugs spewing warnings.
	 */
	if (freq_out)
		ret = wm8994_get_fll_config(&fll, freq_in, freq_out);
	else
		ret = wm8994_get_fll_config(&fll, wm8994->fll[id].in,
					    wm8994->fll[id].out);
	if (ret < 0)
		return ret;

	/* Gate the AIF clocks while we reclock */
	snd_soc_update_bits(codec, WM8994_AIF1_CLOCKING_1,
			    WM8994_AIF1CLK_ENA, 0);
	snd_soc_update_bits(codec, WM8994_AIF2_CLOCKING_1,
			    WM8994_AIF2CLK_ENA, 0);

	/* We always need to disable the FLL while reconfiguring */
	snd_soc_update_bits(codec, WM8994_FLL1_CONTROL_1 + reg_offset,
			    WM8994_FLL1_ENA, 0);

	reg = (fll.outdiv << WM8994_FLL1_OUTDIV_SHIFT) |
		(fll.fll_fratio << WM8994_FLL1_FRATIO_SHIFT);
	snd_soc_update_bits(codec, WM8994_FLL1_CONTROL_2 + reg_offset,
			    WM8994_FLL1_OUTDIV_MASK |
			    WM8994_FLL1_FRATIO_MASK, reg);

	snd_soc_write(codec, WM8994_FLL1_CONTROL_3 + reg_offset, fll.k);

	snd_soc_update_bits(codec, WM8994_FLL1_CONTROL_4 + reg_offset,
			    WM8994_FLL1_N_MASK,
				    fll.n << WM8994_FLL1_N_SHIFT);

	snd_soc_update_bits(codec, WM8994_FLL1_CONTROL_5 + reg_offset,
			    WM8994_FLL1_REFCLK_DIV_MASK |
			    WM8994_FLL1_REFCLK_SRC_MASK,
			    (fll.clk_ref_div << WM8994_FLL1_REFCLK_DIV_SHIFT) |
			    (src - 1));

	/* Clear any pending completion from a previous failure */
	try_wait_for_completion(&wm8994->fll_locked[id]);

	/* Enable (with fractional mode if required) */
	if (freq_out) {
		if (fll.k)
			reg = WM8994_FLL1_ENA | WM8994_FLL1_FRAC;
		else
			reg = WM8994_FLL1_ENA;
		snd_soc_update_bits(codec, WM8994_FLL1_CONTROL_1 + reg_offset,
				    WM8994_FLL1_ENA | WM8994_FLL1_FRAC,
				    reg);

		if (wm8994->fll_locked_irq) {
			timeout = wait_for_completion_timeout(&wm8994->fll_locked[id],
							      msecs_to_jiffies(10));
			if (timeout == 0)
				dev_warn(codec->dev,
					 "Timed out waiting for FLL lock\n");
		} else {
			msleep(5);
		}
	}

	wm8994->fll[id].in = freq_in;
	wm8994->fll[id].out = freq_out;
	wm8994->fll[id].src = src;

	/* Enable any gated AIF clocks */
	snd_soc_update_bits(codec, WM8994_AIF1_CLOCKING_1,
			    WM8994_AIF1CLK_ENA, aif1);
	snd_soc_update_bits(codec, WM8994_AIF2_CLOCKING_1,
			    WM8994_AIF2CLK_ENA, aif2);

	configure_clock(codec);

	return 0;
}

static irqreturn_t wm8994_fll_locked_irq(int irq, void *data)
{
	struct completion *completion = data;

	complete(completion);

	return IRQ_HANDLED;
}

static int opclk_divs[] = { 10, 20, 30, 40, 55, 60, 80, 120, 160 };

static int wm8994_set_fll(struct snd_soc_dai *dai, int id, int src,
			  unsigned int freq_in, unsigned int freq_out)
{
	return _wm8994_set_fll(dai->codec, id, src, freq_in, freq_out);
}

static int wm8994_set_dai_sysclk(struct snd_soc_dai *dai,
		int clk_id, unsigned int freq, int dir)
{
	struct snd_soc_codec *codec = dai->codec;
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
	int i;

	switch (dai->id) {
	case 1:
	case 2:
		break;

	default:
		/* AIF3 shares clocking with AIF1/2 */
		return -EINVAL;
>>>>>>> v3.1
	}

	reg = wm8994_read(codec, WM8994_AIF1_CLOCKING_1);
	reg &= ~WM8994_AIF1CLK_ENA;
	reg &= ~WM8994_AIF1CLK_SRC_MASK;
	wm8994_write(codec, WM8994_AIF1_CLOCKING_1, reg);

	switch (wm8994->sysclk_source) {
	case WM8994_SYSCLK_MCLK:
		dev_dbg(codec->dev, "Using %dHz MCLK\n", wm8994->mclk_rate);

		reg = wm8994_read(codec, WM8994_AIF1_CLOCKING_1);
		reg &= ~WM8994_AIF1CLK_ENA;
		wm8994_write(codec, WM8994_AIF1_CLOCKING_1, reg);

		reg = wm8994_read(codec, WM8994_AIF1_CLOCKING_1);
		reg &= 0x07;

		if (wm8994->mclk_rate > 13500000) {
			reg |= WM8994_AIF1CLK_DIV;
			wm8994->sysclk_rate = wm8994->mclk_rate / 2;
		} else {
			reg &= ~WM8994_AIF1CLK_DIV;
			wm8994->sysclk_rate = wm8994->mclk_rate;
		}
		reg |= WM8994_AIF1CLK_ENA;
		wm8994_write(codec, WM8994_AIF1_CLOCKING_1, reg);

		/* Enable clocks to the Audio core and sysclk of wm8994 */
		reg = wm8994_read(codec, WM8994_CLOCKING_1);
		reg &= ~(WM8994_SYSCLK_SRC_MASK | WM8994_DSP_FSINTCLK_ENA_MASK
				| WM8994_DSP_FS1CLK_ENA_MASK);
		reg |= (WM8994_DSP_FS1CLK_ENA | WM8994_DSP_FSINTCLK_ENA);
		wm8994_write(codec, WM8994_CLOCKING_1, reg);
		break;

	case WM8994_SYSCLK_FLL:
		switch (wm8994->fs) {
		case 8000:
			wm8994_write(codec, WM8994_FLL1_CONTROL_2, 0x2F00);
			wm8994_write(codec, WM8994_FLL1_CONTROL_3, 0x3126);
			wm8994_write(codec, WM8994_FLL1_CONTROL_4, 0x0100);
			wm8994_write(codec, WM8994_FLL1_CONTROL_5, 0x0C88);
			wm8994_write(codec, WM8994_FLL1_CONTROL_1,
				WM8994_FLL1_FRACN_ENA | WM8994_FLL1_ENA);
			break;

		case 11025:
			wm8994_write(codec, WM8994_FLL1_CONTROL_2, 0x1F00);
			wm8994_write(codec, WM8994_FLL1_CONTROL_3, 0x86C2);
			wm8994_write(codec, WM8994_FLL1_CONTROL_5, 0x0C88);
			wm8994_write(codec, WM8994_FLL1_CONTROL_4, 0x00E0);
			wm8994_write(codec, WM8994_FLL1_CONTROL_1,
				WM8994_FLL1_FRACN_ENA | WM8994_FLL1_ENA);
			break;

		case 12000:
			wm8994_write(codec, WM8994_FLL1_CONTROL_2, 0x1F00);
			wm8994_write(codec, WM8994_FLL1_CONTROL_3, 0x3126);
			wm8994_write(codec, WM8994_FLL1_CONTROL_5, 0x0C88);
			wm8994_write(codec, WM8994_FLL1_CONTROL_4, 0x0100);
			wm8994_write(codec, WM8994_FLL1_CONTROL_1,
				WM8994_FLL1_FRACN_ENA | WM8994_FLL1_ENA);
			break;

		case 16000:
			wm8994_write(codec, WM8994_FLL1_CONTROL_2, 0x1900);
			wm8994_write(codec, WM8994_FLL1_CONTROL_3, 0xE23E);
			wm8994_write(codec, WM8994_FLL1_CONTROL_5, 0x0C88);
			wm8994_write(codec, WM8994_FLL1_CONTROL_4, 0x0100);
			wm8994_write(codec, WM8994_FLL1_CONTROL_1,
				WM8994_FLL1_FRACN_ENA | WM8994_FLL1_ENA);
			break;

		case 22050:
			wm8994_write(codec, WM8994_FLL1_CONTROL_2, 0x0F00);
			wm8994_write(codec, WM8994_FLL1_CONTROL_3, 0x86C2);
			wm8994_write(codec, WM8994_FLL1_CONTROL_5, 0x0C88);
			wm8994_write(codec, WM8994_FLL1_CONTROL_4, 0x00E0);
			wm8994_write(codec, WM8994_FLL1_CONTROL_1,
				WM8994_FLL1_FRACN_ENA | WM8994_FLL1_ENA);
			break;

		case 24000:
			wm8994_write(codec, WM8994_FLL1_CONTROL_2, 0x0F00);
			wm8994_write(codec, WM8994_FLL1_CONTROL_3, 0x3126);
			wm8994_write(codec, WM8994_FLL1_CONTROL_5, 0x0C88);
			wm8994_write(codec, WM8994_FLL1_CONTROL_4, 0x0100);
			wm8994_write(codec, WM8994_FLL1_CONTROL_1,
				WM8994_FLL1_FRACN_ENA | WM8994_FLL1_ENA);
			break;

		case 32000:
			wm8994_write(codec, WM8994_FLL1_CONTROL_2, 0x0C00);
			wm8994_write(codec, WM8994_FLL1_CONTROL_3, 0xE23E);
			wm8994_write(codec, WM8994_FLL1_CONTROL_5, 0x0C88);
			wm8994_write(codec, WM8994_FLL1_CONTROL_4, 0x0100);
			wm8994_write(codec, WM8994_FLL1_CONTROL_1,
				WM8994_FLL1_FRACN_ENA | WM8994_FLL1_ENA);
			break;

		case 44100:
			wm8994_write(codec, WM8994_FLL1_CONTROL_2, 0x0700);
			wm8994_write(codec, WM8994_FLL1_CONTROL_3, 0x86C2);
			wm8994_write(codec, WM8994_FLL1_CONTROL_5, 0x0C88);
			wm8994_write(codec, WM8994_FLL1_CONTROL_4, 0x00E0);
			wm8994_write(codec, WM8994_FLL1_CONTROL_1,
				WM8994_FLL1_FRACN_ENA | WM8994_FLL1_ENA);
			break;

		case 48000:
			wm8994_write(codec, WM8994_FLL1_CONTROL_2, 0x0700);
			wm8994_write(codec, WM8994_FLL1_CONTROL_3, 0x3126);
			wm8994_write(codec, WM8994_FLL1_CONTROL_5, 0x0C88);
			wm8994_write(codec, WM8994_FLL1_CONTROL_4, 0x0100);
			wm8994_write(codec, WM8994_FLL1_CONTROL_1,
				WM8994_FLL1_FRACN_ENA | WM8994_FLL1_ENA);
			break;

		default:
			DEBUG_LOG_ERR("Unsupported Frequency\n");
			break;
		}

		reg = wm8994_read(codec, WM8994_AIF1_CLOCKING_1);
		reg |= WM8994_AIF1CLK_ENA;
		reg |= WM8994_AIF1CLK_SRC_FLL1;
		wm8994_write(codec, WM8994_AIF1_CLOCKING_1, reg);

		/* Enable clocks to the Audio core and sysclk of wm8994*/
		reg = wm8994_read(codec, WM8994_CLOCKING_1);
		reg &= ~(WM8994_SYSCLK_SRC_MASK | WM8994_DSP_FSINTCLK_ENA_MASK |
				WM8994_DSP_FS1CLK_ENA_MASK);
		reg |= (WM8994_DSP_FS1CLK_ENA | WM8994_DSP_FSINTCLK_ENA);
		wm8994_write(codec, WM8994_CLOCKING_1, reg);
		break;

	default:
		dev_err(codec->dev, "System clock not configured\n");
		return -EINVAL;
	}

	dev_dbg(codec->dev, "CLK_SYS is %dHz\n", wm8994->sysclk_rate);

	return 0;
}

static int wm8994_set_sysclk(struct snd_soc_dai *codec_dai,
			     int clk_id, unsigned int freq, int dir)
{
	struct snd_soc_codec *codec = codec_dai->codec;
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);

	DEBUG_LOG("clk_id =%d ", clk_id);

	switch (clk_id) {
	case WM8994_SYSCLK_MCLK:
		wm8994->mclk_rate = freq;
		wm8994->sysclk_source = clk_id;
		break;
	case WM8994_SYSCLK_FLL:
		wm8994->sysclk_rate = freq;
		wm8994->sysclk_source = clk_id;
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static int wm8994_set_dai_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct snd_soc_codec *codec = dai->codec;
<<<<<<< HEAD
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
=======
	struct wm8994 *control = codec->control_data;
	int aif1_reg;
	int aif1 = 0;

	switch (dai->id) {
	case 3:
		switch (control->type) {
		case WM8958:
			aif1_reg = WM8958_AIF3_CONTROL_1;
			break;
		default:
			return 0;
		}
	default:
		return 0;
	}

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		break;
	case SNDRV_PCM_FORMAT_S20_3LE:
		aif1 |= 0x20;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		aif1 |= 0x40;
		break;
	case SNDRV_PCM_FORMAT_S32_LE:
		aif1 |= 0x60;
		break;
	default:
		return -EINVAL;
	}

	return snd_soc_update_bits(codec, aif1_reg, WM8994_AIF1_WL_MASK, aif1);
}

static void wm8994_aif_shutdown(struct snd_pcm_substream *substream,
				struct snd_soc_dai *dai)
{
	struct snd_soc_codec *codec = dai->codec;
	int rate_reg = 0;

	switch (dai->id) {
	case 1:
		rate_reg = WM8994_AIF1_RATE;
		break;
	case 2:
		rate_reg = WM8994_AIF1_RATE;
		break;
	default:
		break;
	}

	/* If the DAI is idle then configure the divider tree for the
	 * lowest output rate to save a little power if the clock is
	 * still active (eg, because it is system clock).
	 */
	if (rate_reg && !dai->playback_active && !dai->capture_active)
		snd_soc_update_bits(codec, rate_reg,
				    WM8994_AIF1_SR_MASK |
				    WM8994_AIF1CLK_RATE_MASK, 0x9);
}

static int wm8994_aif_mute(struct snd_soc_dai *codec_dai, int mute)
{
	struct snd_soc_codec *codec = codec_dai->codec;
	int mute_reg;
	int reg;

	switch (codec_dai->id) {
	case 1:
		mute_reg = WM8994_AIF1_DAC1_FILTERS_1;
		break;
	case 2:
		mute_reg = WM8994_AIF2_DAC_FILTERS_1;
		break;
	default:
		return -EINVAL;
	}
>>>>>>> v3.1

	unsigned int aif1 = wm8994_read(codec, WM8994_AIF1_CONTROL_1);
	unsigned int aif2 = wm8994_read(codec, WM8994_AIF1_MASTER_SLAVE);

	DEBUG_LOG("");

	aif1 &= ~(WM8994_AIF1_LRCLK_INV | WM8994_AIF1_BCLK_INV |
			WM8994_AIF1_WL_MASK | WM8994_AIF1_FMT_MASK);

	aif2 &= ~(WM8994_AIF1_LRCLK_FRC_MASK |
			WM8994_AIF1_CLK_FRC | WM8994_AIF1_MSTR);

	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBS_CFS:
		wm8994->master = 0;
		break;
	case SND_SOC_DAIFMT_CBS_CFM:
		aif2 |= (WM8994_AIF1_MSTR | WM8994_AIF1_LRCLK_FRC);
		wm8994->master = 1;
		break;
	case SND_SOC_DAIFMT_CBM_CFS:
		aif2 |= (WM8994_AIF1_MSTR | WM8994_AIF1_CLK_FRC);
		wm8994->master = 1;
		break;
	case SND_SOC_DAIFMT_CBM_CFM:
		aif2 |= (WM8994_AIF1_MSTR | WM8994_AIF1_CLK_FRC |
				WM8994_AIF1_LRCLK_FRC);
		wm8994->master = 1;
		break;
	default:
		return -EINVAL;
	}

<<<<<<< HEAD
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_DSP_B:
		aif1 |= WM8994_AIF1_LRCLK_INV;
	case SND_SOC_DAIFMT_DSP_A:
		aif1 |= 0x18;
		break;
	case SND_SOC_DAIFMT_I2S:
		aif1 |= 0x10;
		break;
	case SND_SOC_DAIFMT_RIGHT_J:
=======
	if (tristate)
		val = mask;
	else
		val = 0;

	return snd_soc_update_bits(codec, reg, mask, val);
}

#define WM8994_RATES SNDRV_PCM_RATE_8000_96000

#define WM8994_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S20_3LE |\
			SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S32_LE)

static struct snd_soc_dai_ops wm8994_aif1_dai_ops = {
	.set_sysclk	= wm8994_set_dai_sysclk,
	.set_fmt	= wm8994_set_dai_fmt,
	.hw_params	= wm8994_hw_params,
	.shutdown	= wm8994_aif_shutdown,
	.digital_mute	= wm8994_aif_mute,
	.set_pll	= wm8994_set_fll,
	.set_tristate	= wm8994_set_tristate,
};

static struct snd_soc_dai_ops wm8994_aif2_dai_ops = {
	.set_sysclk	= wm8994_set_dai_sysclk,
	.set_fmt	= wm8994_set_dai_fmt,
	.hw_params	= wm8994_hw_params,
	.shutdown	= wm8994_aif_shutdown,
	.digital_mute   = wm8994_aif_mute,
	.set_pll	= wm8994_set_fll,
	.set_tristate	= wm8994_set_tristate,
};

static struct snd_soc_dai_ops wm8994_aif3_dai_ops = {
	.hw_params	= wm8994_aif3_hw_params,
	.set_tristate	= wm8994_set_tristate,
};

static struct snd_soc_dai_driver wm8994_dai[] = {
	{
		.name = "wm8994-aif1",
		.id = 1,
		.playback = {
			.stream_name = "AIF1 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = WM8994_RATES,
			.formats = WM8994_FORMATS,
		},
		.capture = {
			.stream_name = "AIF1 Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = WM8994_RATES,
			.formats = WM8994_FORMATS,
		 },
		.ops = &wm8994_aif1_dai_ops,
	},
	{
		.name = "wm8994-aif2",
		.id = 2,
		.playback = {
			.stream_name = "AIF2 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = WM8994_RATES,
			.formats = WM8994_FORMATS,
		},
		.capture = {
			.stream_name = "AIF2 Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = WM8994_RATES,
			.formats = WM8994_FORMATS,
		},
		.ops = &wm8994_aif2_dai_ops,
	},
	{
		.name = "wm8994-aif3",
		.id = 3,
		.playback = {
			.stream_name = "AIF3 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = WM8994_RATES,
			.formats = WM8994_FORMATS,
		},
		.capture = {
			.stream_name = "AIF3 Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = WM8994_RATES,
			.formats = WM8994_FORMATS,
		},
		.ops = &wm8994_aif3_dai_ops,
	}
};

#ifdef CONFIG_PM
static int wm8994_suspend(struct snd_soc_codec *codec, pm_message_t state)
{
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
	struct wm8994 *control = codec->control_data;
	int i, ret;

	switch (control->type) {
	case WM8994:
		snd_soc_update_bits(codec, WM8994_MICBIAS, WM8994_MICD_ENA, 0);
>>>>>>> v3.1
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		aif1 |= 0x8;
		break;
	default:
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_DSP_A:
	case SND_SOC_DAIFMT_DSP_B:
		switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
		case SND_SOC_DAIFMT_NB_NF:
			break;
		case SND_SOC_DAIFMT_IB_NF:
			aif1 |= WM8994_AIF1_BCLK_INV;
			break;
		default:
			return -EINVAL;
		}
		break;

	case SND_SOC_DAIFMT_I2S:
	case SND_SOC_DAIFMT_RIGHT_J:
	case SND_SOC_DAIFMT_LEFT_J:
		switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
		case SND_SOC_DAIFMT_NB_NF:
			break;
		case SND_SOC_DAIFMT_IB_IF:
			aif1 |= WM8994_AIF1_BCLK_INV | WM8994_AIF1_LRCLK_INV;
			break;
		case SND_SOC_DAIFMT_IB_NF:
			aif1 |= WM8994_AIF1_BCLK_INV;
			break;
		case SND_SOC_DAIFMT_NB_IF:
			aif1 |= WM8994_AIF1_LRCLK_INV;
			break;
		default:
			return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}

	aif1 |= 0x4000;
	wm8994_write(codec, WM8994_AIF1_CONTROL_1, aif1);
	wm8994_write(codec, WM8994_AIF1_MASTER_SLAVE, aif2);
	wm8994_write(codec, WM8994_AIF1_CONTROL_2, 0x4000);

	return 0;
}

static int wm8994_hw_params(struct snd_pcm_substream *substream,
			    struct snd_pcm_hw_params *params,
			    struct snd_soc_dai *dai)
{
	struct snd_soc_codec *codec = dai->codec;
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
	int ret, i, best, best_val, cur_val;
	unsigned int clocking1, clocking3, aif1, aif4, aif5;

	DEBUG_LOG("");

	clocking1 = wm8994_read(codec, WM8994_AIF1_BCLK);
	clocking1 &= ~WM8994_AIF1_BCLK_DIV_MASK;

	clocking3 = wm8994_read(codec, WM8994_AIF1_RATE);
	clocking3 &= ~(WM8994_AIF1_SR_MASK | WM8994_AIF1CLK_RATE_MASK);

	aif1 = wm8994_read(codec, WM8994_AIF1_CONTROL_1);
	aif1 &= ~WM8994_AIF1_WL_MASK;
	aif4 = wm8994_read(codec, WM8994_AIF1ADC_LRCLK);
	aif4 &= ~WM8994_AIF1ADC_LRCLK_DIR;
	aif5 = wm8994_read(codec, WM8994_AIF1DAC_LRCLK);
	aif5 &= ~WM8994_AIF1DAC_LRCLK_DIR_MASK;

	wm8994->fs = params_rate(params);
	wm8994->bclk = 2 * wm8994->fs;

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		wm8994->bclk *= 16;
		break;

	case SNDRV_PCM_FORMAT_S20_3LE:
		wm8994->bclk *= 20;
		aif1 |= (0x01 << WM8994_AIF1_WL_SHIFT);
		break;

	case SNDRV_PCM_FORMAT_S24_LE:
		wm8994->bclk *= 24;
		aif1 |= (0x10 << WM8994_AIF1_WL_SHIFT);
		break;

	case SNDRV_PCM_FORMAT_S32_LE:
		wm8994->bclk *= 32;
		aif1 |= (0x11 << WM8994_AIF1_WL_SHIFT);
		break;

	default:
		return -EINVAL;
	}

	ret = configure_clock(codec);
	if (ret != 0)
		return ret;

	dev_dbg(codec->dev, "Target BCLK is %dHz\n", wm8994->bclk);

	/* Select nearest CLK_SYS_RATE */
	if (wm8994->fs == 8000)
		best = 3;
	else {
		best = 0;
		best_val = abs((wm8994->sysclk_rate / clk_sys_rates[0].ratio)
				- wm8994->fs);

		for (i = 1; i < ARRAY_SIZE(clk_sys_rates); i++) {
			cur_val = abs((wm8994->sysclk_rate /
					clk_sys_rates[i].ratio)	- wm8994->fs);

			if (cur_val < best_val) {
				best = i;
				best_val = cur_val;
			}
		}
		dev_dbg(codec->dev, "Selected CLK_SYS_RATIO of %d\n",
				clk_sys_rates[best].ratio);
	}

	clocking3 |= (clk_sys_rates[best].clk_sys_rate
			<< WM8994_AIF1CLK_RATE_SHIFT);

	/* Sampling rate */
	best = 0;
	best_val = abs(wm8994->fs - sample_rates[0].rate);
	for (i = 1; i < ARRAY_SIZE(sample_rates); i++) {
		cur_val = abs(wm8994->fs - sample_rates[i].rate);
		if (cur_val < best_val) {
			best = i;
			best_val = cur_val;
		}
	}
	dev_dbg(codec->dev, "Selected SAMPLE_RATE of %dHz\n",
			sample_rates[best].rate);

	clocking3 |= (sample_rates[best].sample_rate << WM8994_AIF1_SR_SHIFT);

	/* BCLK_DIV */
	best = 0;
	best_val = INT_MAX;
	for (i = 0; i < ARRAY_SIZE(bclk_divs); i++) {
		cur_val = ((wm8994->sysclk_rate) / bclk_divs[i].div)
				  - wm8994->bclk;
		if (cur_val < 0)
			break;
		if (cur_val < best_val) {
			best = i;
			best_val = cur_val;
		}
	}
	wm8994->bclk = (wm8994->sysclk_rate) / bclk_divs[best].div;

	dev_dbg(codec->dev, "Selected BCLK_DIV of %d for %dHz BCLK\n",
			bclk_divs[best].div, wm8994->bclk);

	clocking1 |= bclk_divs[best].bclk_div << WM8994_AIF1_BCLK_DIV_SHIFT;

	/* LRCLK is a simple fraction of BCLK */
	dev_dbg(codec->dev, "LRCLK_RATE is %d\n", wm8994->bclk / wm8994->fs);

	aif4 |= wm8994->bclk / wm8994->fs;
	aif5 |= wm8994->bclk / wm8994->fs;

#ifdef HDMI_USE_AUDIO
	/* set bclk to 32fs for 44.1kHz 16 bit playback.*/
	if (wm8994->fs == 44100)
		wm8994_write(codec, WM8994_AIF1_BCLK, 0x70);
#endif

	wm8994_write(codec, WM8994_AIF1_RATE, clocking3);
	wm8994_write(codec, WM8994_AIF1_CONTROL_1, aif1);

	return 0;
}

static int wm8994_digital_mute(struct snd_soc_dai *codec_dai, int mute)
{
	struct snd_soc_codec *codec = codec_dai->codec;
	int mute_reg;
	int reg;

	switch (codec_dai->id) {
	case 1:
		mute_reg = WM8994_AIF1_DAC1_FILTERS_1;
		break;
	case 2:
		mute_reg = WM8994_AIF2_DAC_FILTERS_1;
		break;
	default:
		return -EINVAL;
	}

	if (mute)
		reg = WM8994_AIF1DAC1_MUTE;
	else
		reg = 0;

	snd_soc_update_bits(codec, mute_reg, WM8994_AIF1DAC1_MUTE, reg);

	return 0;
}

static int wm8994_startup(struct snd_pcm_substream *substream,
			  struct snd_soc_dai *codec_dai)
{
	struct snd_soc_codec *codec = codec_dai->codec;
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		wm8994->stream_state |=  PCM_STREAM_PLAYBACK;
	else
		wm8994->stream_state |= PCM_STREAM_CAPTURE;


	if (wm8994->power_state == CODEC_OFF) {
		if (!get_audio_power_status()) {
			DEBUG_LOG("Power on codec");
			audio_power(1);
		}

		wm8994->power_state = CODEC_ON;
		DEBUG_LOG("Turn on codec!! Power state =[%d]",
				wm8994->power_state);

		/* For initialize codec */
		wm8994_write(codec, WM8994_POWER_MANAGEMENT_1,
				0x3 << WM8994_VMID_SEL_SHIFT | WM8994_BIAS_ENA);
		msleep(50);
		wm8994_write(codec, WM8994_POWER_MANAGEMENT_1,
				WM8994_VMID_SEL_NORMAL | WM8994_BIAS_ENA);
		wm8994_write(codec, WM8994_OVERSAMPLING, 0x0000);
	} else
		DEBUG_LOG("Already turned on codec!!");

	return 0;
}

static void wm8994_shutdown(struct snd_pcm_substream *substream,
			    struct snd_soc_dai *codec_dai)
{
	wm8994_shutdown_codec(substream, codec_dai->codec);
}

static void wm8994_shutdown_codec(struct snd_pcm_substream *substream,
			    struct snd_soc_codec *codec)
{
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);

	if (wm8994->testmode_config_flag == SEC_TEST_HWCODEC) {
		DEBUG_LOG_ERR("SEC_TEST_HWCODEC is activated!! Don't shutdown(reset) sequence!!");
		return;
	}

	DEBUG_LOG("Stream_state = [0x%X],  Codec State = [0x%X]",
			wm8994->stream_state, wm8994->codec_state);

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		wm8994->stream_state &=  ~(PCM_STREAM_CAPTURE);
		wm8994->codec_state &= ~(CAPTURE_ACTIVE);
	} else {
		wm8994->codec_state &= ~(PLAYBACK_ACTIVE);
		wm8994->stream_state &= ~(PCM_STREAM_PLAYBACK);
	}

	if ((wm8994->codec_state == DEACTIVE) &&
			(wm8994->stream_state == PCM_STREAM_DEACTIVE)) {
		DEBUG_LOG("Turn off Codec!!");
		audio_ctrl_mic_bias_gpio(0);
		wm8994->power_state = CODEC_OFF;
		wm8994->cur_path = PLAYBACK_OFF;
		wm8994->rec_path = MIC_OFF;
		wm8994->fmradio_path = FMR_OFF;
		wm8994->fmr_mix_path = FMR_MIX_OFF;
		wm8994->ringtone_active = OFF;
		wm8994->call_record_path = CALL_RECORDING_OFF;
		wm8994->call_record_ch = CH_OFF;
		wm8994_write(codec, WM8994_SOFTWARE_RESET, 0x0000);
		return;
	}

	DEBUG_LOG("Preserve codec state = [0x%X], Stream State = [0x%X]",
			wm8994->codec_state, wm8994->stream_state);

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		wm8994_disable_rec_path(codec, wm8994->rec_path);
		wm8994->codec_state &= ~(CAPTURE_ACTIVE);
	} else {
		if (wm8994->codec_state & CALL_ACTIVE) {
			int val;

			val = wm8994_read(codec, WM8994_AIF1_DAC1_FILTERS_1);
			val &= ~(WM8994_AIF1DAC1_MUTE_MASK);
			val |= (WM8994_AIF1DAC1_MUTE);
			wm8994_write(codec, WM8994_AIF1_DAC1_FILTERS_1, val);
		} else if (wm8994->codec_state & CAPTURE_ACTIVE) {
			wm8994_disable_playback_path(codec, wm8994->cur_path);
		} else if (wm8994->codec_state & FMRADIO_ACTIVE) {
			// FM radio deactive
			int val;

			val = wm8994_read(codec, WM8994_AIF1_DAC1_FILTERS_1);
			val &= ~(WM8994_AIF1DAC1_MUTE_MASK);
			val |= (WM8994_AIF1DAC1_MUTE);
			wm8994_write(codec, WM8994_AIF1_DAC1_FILTERS_1, val);
		} else {
			// Playback deactive
		}
	}
}

//static struct snd_soc_device *wm8994_socdev;
static struct snd_soc_codec *wm8994_codec;

#define WM8994_RATES SNDRV_PCM_RATE_8000_96000
#define WM8994_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S20_3LE |\
			SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S32_LE)
static struct snd_soc_dai_ops wm8994_ops = {
	.startup = wm8994_startup,
	.shutdown = wm8994_shutdown,
	.set_sysclk = wm8994_set_sysclk,
	.set_fmt = wm8994_set_dai_fmt,
	.hw_params = wm8994_hw_params,
	.digital_mute = wm8994_digital_mute,
};

struct snd_soc_dai_driver wm8994_dai[] = {
	{
		.name = "WM8994 PAIFRX",
		.playback = {
			.stream_name = "Playback",
			.channels_min = 1,
			.channels_max = 6,
			.rates = WM8994_RATES,
			.formats = WM8994_FORMATS,
		},
		.capture = {
			.stream_name = "Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = WM8994_RATES,
			.formats = WM8994_FORMATS,
		},
		.ops = &wm8994_ops,
	},
};

<<<<<<< HEAD
/*
 * initialise the WM8994 driver
 * register the mixer and dsp interfaces with the kernel
 */
static int wm8994_init(struct wm8994_priv *wm8994_private)
=======
static irqreturn_t wm8994_fifo_error(int irq, void *data)
{
	struct snd_soc_codec *codec = data;

	dev_err(codec->dev, "FIFO error\n");

	return IRQ_HANDLED;
}

static int wm8994_codec_probe(struct snd_soc_codec *codec)
>>>>>>> v3.1
{
	struct snd_soc_codec *codec = wm8994_private->codec;
	struct wm8994_priv *wm8994;
	int ret = 0;
	DEBUG_LOG("");
	wm8994 = kzalloc(sizeof(struct wm8994_priv), GFP_KERNEL);
	if (wm8994 == NULL)
		return -ENOMEM;

	snd_soc_codec_set_drvdata(codec, wm8994);

<<<<<<< HEAD
	wm8994->universal_playback_path = universal_wm8994_playback_paths;
	wm8994->universal_voicecall_path = universal_wm8994_voicecall_paths;
	wm8994->universal_mic_path = universal_wm8994_mic_paths;
	wm8994->stream_state = PCM_STREAM_DEACTIVE;
	wm8994->codec_state = DEACTIVE;
	wm8994->cur_path = PLAYBACK_OFF;
	wm8994->rec_path = MIC_OFF;
	wm8994->call_record_path = CALL_RECORDING_OFF;
	wm8994->call_record_ch = CH_OFF;
	wm8994->fmradio_path = FMR_OFF;
	wm8994->fmr_mix_path = FMR_MIX_OFF;
	wm8994->testmode_config_flag = SEC_NORMAL;
	wm8994->power_state = CODEC_OFF;
	wm8994->recognition_active = REC_OFF;
#ifdef FEATURE_VSUITE_RECOGNITION
	wm8994->vsuite_recognition_active = REC_OFF;
#endif
	wm8994->ringtone_active = OFF;
	wm8994_write(codec, WM8994_SOFTWARE_RESET, 0x0000);
	wm8994_write(codec, WM8994_POWER_MANAGEMENT_1,
		0x3 << WM8994_VMID_SEL_SHIFT | WM8994_BIAS_ENA);

	msleep(50);
=======
	wm8994->pdata = dev_get_platdata(codec->dev->parent);
	wm8994->codec = codec;

	for (i = 0; i < ARRAY_SIZE(wm8994->fll_locked); i++)
		init_completion(&wm8994->fll_locked[i]);

	if (wm8994->pdata && wm8994->pdata->micdet_irq)
		wm8994->micdet_irq = wm8994->pdata->micdet_irq;
	else if (wm8994->pdata && wm8994->pdata->irq_base)
		wm8994->micdet_irq = wm8994->pdata->irq_base +
				     WM8994_IRQ_MIC1_DET;

	pm_runtime_enable(codec->dev);
	pm_runtime_resume(codec->dev);

	/* Read our current status back from the chip - we don't want to
	 * reset as this may interfere with the GPIO or LDO operation. */
	for (i = 0; i < WM8994_CACHE_SIZE; i++) {
		if (!wm8994_readable(codec, i) || wm8994_volatile(codec, i))
			continue;

		ret = wm8994_reg_read(codec->control_data, i);
		if (ret <= 0)
			continue;

		ret = snd_soc_cache_write(codec, i, ret);
		if (ret != 0) {
			dev_err(codec->dev,
				"Failed to initialise cache for 0x%x: %d\n",
				i, ret);
			goto err;
		}
	}

	/* Set revision-specific configuration */
	wm8994->revision = snd_soc_read(codec, WM8994_CHIP_REVISION);
	switch (control->type) {
	case WM8994:
		switch (wm8994->revision) {
		case 2:
		case 3:
			wm8994->hubs.dcs_codes = -5;
			wm8994->hubs.hp_startup_mode = 1;
			wm8994->hubs.dcs_readback_mode = 1;
			wm8994->hubs.series_startup = 1;
			break;
		default:
			wm8994->hubs.dcs_readback_mode = 1;
			break;
		}
		break;
>>>>>>> v3.1

	wm8994_write(codec, WM8994_POWER_MANAGEMENT_1,
		WM8994_VMID_SEL_NORMAL | WM8994_BIAS_ENA);
	wm8994->hw_version = wm8994_read(codec, 0x100);
	wm8994_codec = codec;
	wm8994_add_controls(codec);
	wm8994_add_widgets(codec);

	return ret;
}

<<<<<<< HEAD
/* If the i2c layer weren't so broken, we could pass this kind of data
   around */
=======
	wm8994_request_irq(codec->control_data, WM8994_IRQ_FIFOS_ERR,
			   wm8994_fifo_error, "FIFO error", codec);

	ret = wm8994_request_irq(codec->control_data, WM8994_IRQ_DCS_DONE,
				 wm_hubs_dcs_done, "DC servo done",
				 &wm8994->hubs);
	if (ret == 0)
		wm8994->hubs.dcs_done_irq = true;

	switch (control->type) {
	case WM8994:
		if (wm8994->micdet_irq) {
			ret = request_threaded_irq(wm8994->micdet_irq, NULL,
						   wm8994_mic_irq,
						   IRQF_TRIGGER_RISING,
						   "Mic1 detect",
						   wm8994);
			if (ret != 0)
				dev_warn(codec->dev,
					 "Failed to request Mic1 detect IRQ: %d\n",
					 ret);
		}
>>>>>>> v3.1

static int wm8994_codec_probe(struct snd_soc_codec *codec)
{
	struct wm8994_priv *wm8994_priv;
	int ret = -ENODEV;

	DEBUG_LOG("");
	pr_info("WM8994 Audio Codec %s\n", WM8994_VERSION);

<<<<<<< HEAD
	wm8994_priv = kzalloc(sizeof(struct wm8994_priv), GFP_KERNEL);
	if (wm8994_priv == NULL)
		return -ENOMEM;
=======
	wm8994->fll_locked_irq = true;
	for (i = 0; i < ARRAY_SIZE(wm8994->fll_locked); i++) {
		ret = wm8994_request_irq(codec->control_data,
					 WM8994_IRQ_FLL1_LOCK + i,
					 wm8994_fll_locked_irq, "FLL lock",
					 &wm8994->fll_locked[i]);
		if (ret != 0)
			wm8994->fll_locked_irq = false;
	}

	/* Remember if AIFnLRCLK is configured as a GPIO.  This should be
	 * configured on init - if a system wants to do this dynamically
	 * at runtime we can deal with that then.
	 */
	ret = wm8994_reg_read(codec->control_data, WM8994_GPIO_1);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to read GPIO1 state: %d\n", ret);
		goto err_irq;
	}
	if ((ret & WM8994_GPN_FN_MASK) != WM8994_GP_FN_PIN_SPECIFIC) {
		wm8994->lrclk_shared[0] = 1;
		wm8994_dai[0].symmetric_rates = 1;
	} else {
		wm8994->lrclk_shared[0] = 0;
	}
>>>>>>> v3.1

	wm8994_priv->codec = codec;
#ifdef PM_DEBUG
	pm_codec = codec;
#endif

<<<<<<< HEAD
	codec->hw_write = (hw_write_t) i2c_master_send;
	codec->control_data = to_i2c_client(codec->dev);
=======
	wm8994_set_bias_level(codec, SND_SOC_BIAS_STANDBY);

	/* Latch volume updates (right only; we always do left then right). */
	snd_soc_update_bits(codec, WM8994_AIF1_DAC1_LEFT_VOLUME,
			    WM8994_AIF1DAC1_VU, WM8994_AIF1DAC1_VU);
	snd_soc_update_bits(codec, WM8994_AIF1_DAC1_RIGHT_VOLUME,
			    WM8994_AIF1DAC1_VU, WM8994_AIF1DAC1_VU);
	snd_soc_update_bits(codec, WM8994_AIF1_DAC2_LEFT_VOLUME,
			    WM8994_AIF1DAC2_VU, WM8994_AIF1DAC2_VU);
	snd_soc_update_bits(codec, WM8994_AIF1_DAC2_RIGHT_VOLUME,
			    WM8994_AIF1DAC2_VU, WM8994_AIF1DAC2_VU);
	snd_soc_update_bits(codec, WM8994_AIF2_DAC_LEFT_VOLUME,
			    WM8994_AIF2DAC_VU, WM8994_AIF2DAC_VU);
	snd_soc_update_bits(codec, WM8994_AIF2_DAC_RIGHT_VOLUME,
			    WM8994_AIF2DAC_VU, WM8994_AIF2DAC_VU);
	snd_soc_update_bits(codec, WM8994_AIF1_ADC1_LEFT_VOLUME,
			    WM8994_AIF1ADC1_VU, WM8994_AIF1ADC1_VU);
	snd_soc_update_bits(codec, WM8994_AIF1_ADC1_RIGHT_VOLUME,
			    WM8994_AIF1ADC1_VU, WM8994_AIF1ADC1_VU);
	snd_soc_update_bits(codec, WM8994_AIF1_ADC2_LEFT_VOLUME,
			    WM8994_AIF1ADC2_VU, WM8994_AIF1ADC2_VU);
	snd_soc_update_bits(codec, WM8994_AIF1_ADC2_RIGHT_VOLUME,
			    WM8994_AIF1ADC2_VU, WM8994_AIF1ADC2_VU);
	snd_soc_update_bits(codec, WM8994_AIF2_ADC_LEFT_VOLUME,
			    WM8994_AIF2ADC_VU, WM8994_AIF1ADC2_VU);
	snd_soc_update_bits(codec, WM8994_AIF2_ADC_RIGHT_VOLUME,
			    WM8994_AIF2ADC_VU, WM8994_AIF1ADC2_VU);
	snd_soc_update_bits(codec, WM8994_DAC1_LEFT_VOLUME,
			    WM8994_DAC1_VU, WM8994_DAC1_VU);
	snd_soc_update_bits(codec, WM8994_DAC1_RIGHT_VOLUME,
			    WM8994_DAC1_VU, WM8994_DAC1_VU);
	snd_soc_update_bits(codec, WM8994_DAC2_LEFT_VOLUME,
			    WM8994_DAC2_VU, WM8994_DAC2_VU);
	snd_soc_update_bits(codec, WM8994_DAC2_RIGHT_VOLUME,
			    WM8994_DAC2_VU, WM8994_DAC2_VU);

	/* Set the low bit of the 3D stereo depth so TLV matches */
	snd_soc_update_bits(codec, WM8994_AIF1_DAC1_FILTERS_2,
			    1 << WM8994_AIF1DAC1_3D_GAIN_SHIFT,
			    1 << WM8994_AIF1DAC1_3D_GAIN_SHIFT);
	snd_soc_update_bits(codec, WM8994_AIF1_DAC2_FILTERS_2,
			    1 << WM8994_AIF1DAC2_3D_GAIN_SHIFT,
			    1 << WM8994_AIF1DAC2_3D_GAIN_SHIFT);
	snd_soc_update_bits(codec, WM8994_AIF2_DAC_FILTERS_2,
			    1 << WM8994_AIF2DAC_3D_GAIN_SHIFT,
			    1 << WM8994_AIF2DAC_3D_GAIN_SHIFT);

	/* Unconditionally enable AIF1 ADC TDM mode on chips which can
	 * use this; it only affects behaviour on idle TDM clock
	 * cycles. */
	switch (control->type) {
	case WM8994:
	case WM8958:
		snd_soc_update_bits(codec, WM8994_AIF1_CONTROL_1,
				    WM8994_AIF1ADC_TDM, WM8994_AIF1ADC_TDM);
		break;
	default:
		break;
	}

	wm8994_update_class_w(codec);

	wm8994_handle_pdata(wm8994);

	wm_hubs_add_analogue_controls(codec);
	snd_soc_add_controls(codec, wm8994_snd_controls,
			     ARRAY_SIZE(wm8994_snd_controls));
	snd_soc_dapm_new_controls(dapm, wm8994_dapm_widgets,
				  ARRAY_SIZE(wm8994_dapm_widgets));

	switch (control->type) {
	case WM8994:
		snd_soc_dapm_new_controls(dapm, wm8994_specific_dapm_widgets,
					  ARRAY_SIZE(wm8994_specific_dapm_widgets));
		if (wm8994->revision < 4) {
			snd_soc_dapm_new_controls(dapm, wm8994_lateclk_revd_widgets,
						  ARRAY_SIZE(wm8994_lateclk_revd_widgets));
			snd_soc_dapm_new_controls(dapm, wm8994_adc_revd_widgets,
						  ARRAY_SIZE(wm8994_adc_revd_widgets));
			snd_soc_dapm_new_controls(dapm, wm8994_dac_revd_widgets,
						  ARRAY_SIZE(wm8994_dac_revd_widgets));
		} else {
			snd_soc_dapm_new_controls(dapm, wm8994_lateclk_widgets,
						  ARRAY_SIZE(wm8994_lateclk_widgets));
			snd_soc_dapm_new_controls(dapm, wm8994_adc_widgets,
						  ARRAY_SIZE(wm8994_adc_widgets));
			snd_soc_dapm_new_controls(dapm, wm8994_dac_widgets,
						  ARRAY_SIZE(wm8994_dac_widgets));
		}
		break;
	case WM8958:
		snd_soc_add_controls(codec, wm8958_snd_controls,
				     ARRAY_SIZE(wm8958_snd_controls));
		snd_soc_dapm_new_controls(dapm, wm8958_dapm_widgets,
					  ARRAY_SIZE(wm8958_dapm_widgets));
		if (wm8994->revision < 1) {
			snd_soc_dapm_new_controls(dapm, wm8994_lateclk_revd_widgets,
						  ARRAY_SIZE(wm8994_lateclk_revd_widgets));
			snd_soc_dapm_new_controls(dapm, wm8994_adc_revd_widgets,
						  ARRAY_SIZE(wm8994_adc_revd_widgets));
			snd_soc_dapm_new_controls(dapm, wm8994_dac_revd_widgets,
						  ARRAY_SIZE(wm8994_dac_revd_widgets));
		} else {
			snd_soc_dapm_new_controls(dapm, wm8994_lateclk_widgets,
						  ARRAY_SIZE(wm8994_lateclk_widgets));
			snd_soc_dapm_new_controls(dapm, wm8994_adc_widgets,
						  ARRAY_SIZE(wm8994_adc_widgets));
			snd_soc_dapm_new_controls(dapm, wm8994_dac_widgets,
						  ARRAY_SIZE(wm8994_dac_widgets));
		}
		break;
	}
		
>>>>>>> v3.1

	ret = wm8994_init(wm8994_priv);
#ifdef CONFIG_SND_WM8994_EXTENSIONS
	wm8994_extensions_pcm_probe(codec);
#endif
	if (ret)
		dev_err(codec->dev, "failed to initialize WM8994\n");

	return ret;
}

static int  wm8994_codec_remove(struct snd_soc_codec *codec)
{
	struct wm8994_priv *wm8994_priv = snd_soc_codec_get_drvdata(codec);
	kfree(wm8994_priv);
	return 0;
}

#ifdef CONFIG_PM
static int wm8994_suspend(struct snd_soc_codec *codec, pm_message_t state)
{
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);

	DEBUG_LOG("Codec State = [0x%X], Stream State = [0x%X]",
			wm8994->codec_state, wm8994->stream_state);

	if (wm8994->testmode_config_flag == SEC_TEST_HWCODEC) {
		DEBUG_LOG_ERR("SEC_TEST_HWCODEC is activated!! Skip suspend sequence!!");
		return 0;
	}

	if (wm8994->codec_state == DEACTIVE &&
		wm8994->stream_state == PCM_STREAM_DEACTIVE) {
		wm8994->power_state = CODEC_OFF;
		wm8994_write(codec, WM8994_SOFTWARE_RESET, 0x0000);
		audio_power(0);
	}

<<<<<<< HEAD
	return 0;
=======
err_irq:
	wm8994_free_irq(codec->control_data, WM8994_IRQ_MIC2_SHRT, wm8994);
	wm8994_free_irq(codec->control_data, WM8994_IRQ_MIC2_DET, wm8994);
	wm8994_free_irq(codec->control_data, WM8994_IRQ_MIC1_SHRT, wm8994);
	if (wm8994->micdet_irq)
		free_irq(wm8994->micdet_irq, wm8994);
	for (i = 0; i < ARRAY_SIZE(wm8994->fll_locked); i++)
		wm8994_free_irq(codec->control_data, WM8994_IRQ_FLL1_LOCK + i,
				&wm8994->fll_locked[i]);
	wm8994_free_irq(codec->control_data, WM8994_IRQ_DCS_DONE,
			&wm8994->hubs);
	wm8994_free_irq(codec->control_data, WM8994_IRQ_FIFOS_ERR, codec);
err:
	kfree(wm8994);
	return ret;
>>>>>>> v3.1
}

static int wm8994_resume(struct snd_soc_codec *codec)
{
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
<<<<<<< HEAD
=======
	struct wm8994 *control = codec->control_data;
	int i;

	wm8994_set_bias_level(codec, SND_SOC_BIAS_OFF);

	pm_runtime_disable(codec->dev);

	for (i = 0; i < ARRAY_SIZE(wm8994->fll_locked); i++)
		wm8994_free_irq(codec->control_data, WM8994_IRQ_FLL1_LOCK + i,
				&wm8994->fll_locked[i]);

	wm8994_free_irq(codec->control_data, WM8994_IRQ_DCS_DONE,
			&wm8994->hubs);
	wm8994_free_irq(codec->control_data, WM8994_IRQ_FIFOS_ERR, codec);

	switch (control->type) {
	case WM8994:
		if (wm8994->micdet_irq)
			free_irq(wm8994->micdet_irq, wm8994);
		wm8994_free_irq(codec->control_data, WM8994_IRQ_MIC2_DET,
				wm8994);
		wm8994_free_irq(codec->control_data, WM8994_IRQ_MIC1_SHRT,
				wm8994);
		wm8994_free_irq(codec->control_data, WM8994_IRQ_MIC1_DET,
				wm8994);
		break;
>>>>>>> v3.1

	DEBUG_LOG("%s..\n", __func__);
	DEBUG_LOG_ERR("------WM8994 Revision = [%d]-------\n",
		      wm8994->hw_version);

	if (wm8994->testmode_config_flag == SEC_TEST_HWCODEC) {
		DEBUG_LOG_ERR("SEC_TEST_HWCODEC is activated!! Skip resume sequence!!");
		return 0;
	}

	if (wm8994->power_state == CODEC_OFF) {
		// Turn on sequence by recommend Wolfson.
		audio_power(1);
		wm8994->power_state = CODEC_ON;
		wm8994_write(codec, WM8994_POWER_MANAGEMENT_1,
			0x3 << WM8994_VMID_SEL_SHIFT | WM8994_BIAS_ENA);
		msleep(50);	// Wait to setup PLL.
		wm8994_write(codec, WM8994_POWER_MANAGEMENT_1, 
			WM8994_VMID_SEL_NORMAL | WM8994_BIAS_ENA);
		wm8994_write(codec,WM8994_OVERSAMPLING, 0x0000);
	}
	return 0;
}
#endif

static struct snd_soc_codec_driver soc_codec_dev_wm8994 = {
	.probe =	wm8994_codec_probe,
	.remove =	wm8994_codec_remove,
#ifdef CONFIG_PM
	.suspend =	wm8994_suspend,
	.resume =	wm8994_resume,
#endif
	.read =		wm8994_read,
	.write =	wm8994_write,
	.reg_cache_size = WM8994_IRQ_POLARITY,
	.reg_word_size = 2,
	.compress_type = SND_SOC_RBTREE_COMPRESSION,
};

static int wm8994_i2c_probe(struct i2c_client *client,
			    const struct i2c_device_id *id)
{
	/* Board Specific Function */
	audio_init();
	audio_power(1);
	msleep(10);

	return snd_soc_register_codec(&client->dev, &soc_codec_dev_wm8994,
			wm8994_dai, ARRAY_SIZE(wm8994_dai));
}

static int wm8994_i2c_remove(struct i2c_client *client)
{
	snd_soc_unregister_codec(&client->dev);
	return 0;
}


static const struct i2c_device_id wm8994_i2c_id[] = {
	{"wm8994-samsung", 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, wm8994_i2c_id);

static struct i2c_driver wm8994_i2c_driver = {
	.driver = {
		   .name = "wm8994-samsung-codec",
		   .owner = THIS_MODULE,
		   },
	.probe = wm8994_i2c_probe,
	.remove = wm8994_i2c_remove,
	.id_table = wm8994_i2c_id,
};

static __init int wm8994_driver_init(void)
{
	return i2c_add_driver(&wm8994_i2c_driver);
}
module_init(wm8994_driver_init);

static __exit void wm8994_driver_exit(void)
{
	i2c_del_driver(&wm8994_i2c_driver);
}
module_exit(wm8994_driver_exit);

MODULE_DESCRIPTION("ASoC WM8994 driver");
MODULE_AUTHOR("Shaju Abraham shaju.abraham@samsung.com");
MODULE_LICENSE("GPL");
