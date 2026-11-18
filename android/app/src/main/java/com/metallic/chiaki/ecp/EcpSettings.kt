// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package com.metallic.chiaki.ecp

import android.content.Context
import androidx.preference.PreferenceManager
import com.metallic.chiaki.common.Preferences
import org.json.JSONArray
import org.json.JSONObject

data class SetSettingResult(
	val applied: String? = null,
	val error: String? = null
)

object EcpSettings
{
	const val KEY_AUDIO_VOLUME = "audio.volume"
	const val KEY_VIDEO_RESOLUTION = "video.resolution"
	const val KEY_VIDEO_FPS = "video.fps"
	const val KEY_VIDEO_CODEC = "video.codec"
	const val KEY_VIDEO_BITRATE = "video.bitrate"
	const val KEY_INPUT_RUMBLE = "input.rumble"
	const val KEY_INPUT_MOTION = "input.motion"
	const val KEY_INPUT_SWAP_CROSS_MOON = "input.swap_cross_moon"

	private const val PREF_ECP_AUDIO_VOLUME = "ecp_audio_volume"

	fun getAudioVolume(context: Context): Int
	{
		val sp = PreferenceManager.getDefaultSharedPreferences(context)
		return sp.getInt(PREF_ECP_AUDIO_VOLUME, 100).coerceIn(0, 100)
	}

	fun setAudioVolume(context: Context, percent: Int)
	{
		val clamped = percent.coerceIn(0, 100)
		PreferenceManager.getDefaultSharedPreferences(context)
			.edit()
			.putInt(PREF_ECP_AUDIO_VOLUME, clamped)
			.apply()
	}

	fun getSchema(): JSONArray = JSONArray().apply {
		put(JSONObject().apply {
			put("key", KEY_AUDIO_VOLUME)
			put("label", "Audio Volume (%)")
			put("help", "Remote Play stream volume from 0% to 100%.")
			put("type", "range")
			put("min", 0)
			put("max", 100)
			put("step", 5)
			put("default", 100)
			put("group", "Audio")
			put("applies", "now")
		})
		put(JSONObject().apply {
			put("key", KEY_VIDEO_RESOLUTION)
			put("label", "Stream Resolution")
			put("help", "Target video height requested from the PlayStation console.")
			put("type", "choice")
			put("choices", JSONArray().apply {
				put(choice("360p", "360p"))
				put(choice("540p", "540p"))
				put(choice("720p", "720p"))
				put(choice("1080p", "1080p"))
			})
			put("default", Preferences.resolutionDefault.value)
			put("group", "Video")
			put("applies", "restart")
		})
		put(JSONObject().apply {
			put("key", KEY_VIDEO_FPS)
			put("label", "Frame Rate")
			put("help", "Maximum frames per second requested from the console.")
			put("type", "choice")
			put("choices", JSONArray().apply {
				put(choice("30", "30 FPS"))
				put(choice("60", "60 FPS"))
			})
			put("default", Preferences.fpsDefault.value)
			put("group", "Video")
			put("applies", "restart")
		})
		put(JSONObject().apply {
			put("key", KEY_VIDEO_CODEC)
			put("label", "Video Codec")
			put("help", "Video compression format used by the stream.")
			put("type", "choice")
			put("choices", JSONArray().apply {
				put(choice("h264", "H.264"))
				put(choice("h265", "H.265 (HEVC)"))
			})
			put("default", Preferences.codecDefault.value)
			put("group", "Video")
			put("applies", "restart")
		})
		put(JSONObject().apply {
			put("key", KEY_VIDEO_BITRATE)
			put("label", "Bitrate in kbps (0 = Auto)")
			put("help", "0 lets Chiaki choose a bitrate for the selected resolution and frame rate; otherwise 2000 to 50000 kbps.")
			put("type", "range")
			put("min", 0)
			put("max", 50000)
			put("step", 1000)
			put("default", 0)
			put("group", "Video")
			put("applies", "restart")
		})
		put(JSONObject().apply {
			put("key", KEY_INPUT_RUMBLE)
			put("label", "Controller Rumble")
			put("type", "toggle")
			put("default", true)
			put("group", "Input")
			put("applies", "now")
		})
		put(JSONObject().apply {
			put("key", KEY_INPUT_MOTION)
			put("label", "Gyro / Accelerometer Motion")
			put("type", "toggle")
			put("default", true)
			put("group", "Input")
			put("applies", "restart")
		})
		put(JSONObject().apply {
			put("key", KEY_INPUT_SWAP_CROSS_MOON)
			put("label", "Swap Cross / Circle")
			put("type", "toggle")
			put("default", false)
			put("group", "Input")
			put("applies", "restart")
		})
	}

	fun getValues(context: Context): JSONObject
	{
		val prefs = Preferences(context)
		return JSONObject().apply {
			put(KEY_AUDIO_VOLUME, getAudioVolume(context))
			put(KEY_VIDEO_RESOLUTION, prefs.resolution.value)
			put(KEY_VIDEO_FPS, prefs.fps.value)
			put(KEY_VIDEO_CODEC, prefs.codec.value)
			put(KEY_VIDEO_BITRATE, prefs.bitrate ?: 0)
			put(KEY_INPUT_RUMBLE, prefs.rumbleEnabled)
			put(KEY_INPUT_MOTION, prefs.motionEnabled)
			put(KEY_INPUT_SWAP_CROSS_MOON, prefs.swapCrossMoon)
		}
	}

	fun setValue(context: Context, key: String, rawValue: Any?): SetSettingResult
	{
		if(rawValue == null || rawValue === JSONObject.NULL)
			return SetSettingResult(error = "Missing value for setting '$key'")

		val prefs = Preferences(context)
		return when(key)
		{
			KEY_AUDIO_VOLUME -> {
				val n = (rawValue as? Number)?.toInt()
					?: return SetSettingResult(error = "Setting '$key' expects a number in 0..100")
				if(n < 0 || n > 100)
					return SetSettingResult(error = "Setting '$key' out of range 0..100: $n")
				setAudioVolume(context, n)
				SetSettingResult(applied = "now")
			}
			KEY_VIDEO_RESOLUTION -> {
				val s = rawValue as? String
					?: return SetSettingResult(error = "Setting '$key' expects a string")
				val match = Preferences.Resolution.values().firstOrNull { it.value == s }
					?: return SetSettingResult(error = "Unsupported resolution '$s'")
				prefs.resolution = match
				SetSettingResult(applied = "restart")
			}
			KEY_VIDEO_FPS -> {
				val s = rawValue as? String
					?: return SetSettingResult(error = "Setting '$key' expects a string")
				val match = Preferences.FPS.values().firstOrNull { it.value == s }
					?: return SetSettingResult(error = "Unsupported FPS '$s'")
				prefs.fps = match
				SetSettingResult(applied = "restart")
			}
			KEY_VIDEO_CODEC -> {
				val s = rawValue as? String
					?: return SetSettingResult(error = "Setting '$key' expects a string")
				val match = Preferences.Codec.values().firstOrNull { it.value == s }
					?: return SetSettingResult(error = "Unsupported codec '$s'")
				prefs.codec = match
				SetSettingResult(applied = "restart")
			}
			KEY_VIDEO_BITRATE -> {
				val n = (rawValue as? Number)?.toInt()
					?: return SetSettingResult(error = "Setting '$key' expects an integer")
				if(n < 0 || n > 50000)
					return SetSettingResult(error = "Setting '$key' out of range 0..50000: $n")
				prefs.bitrate = if(n == 0) null else n
				SetSettingResult(applied = "restart")
			}
			KEY_INPUT_RUMBLE -> {
				val b = rawValue as? Boolean
					?: return SetSettingResult(error = "Setting '$key' expects a boolean")
				prefs.rumbleEnabled = b
				SetSettingResult(applied = "now")
			}
			KEY_INPUT_MOTION -> {
				val b = rawValue as? Boolean
					?: return SetSettingResult(error = "Setting '$key' expects a boolean")
				prefs.motionEnabled = b
				SetSettingResult(applied = "restart")
			}
			KEY_INPUT_SWAP_CROSS_MOON -> {
				val b = rawValue as? Boolean
					?: return SetSettingResult(error = "Setting '$key' expects a boolean")
				prefs.swapCrossMoon = b
				SetSettingResult(applied = "restart")
			}
			else -> SetSettingResult(error = "Unknown setting '$key'")
		}
	}

	fun applyConfigure(context: Context, settings: JSONObject?): String?
	{
		if(settings == null)
			return null
		val keys = settings.keys()
		while(keys.hasNext())
		{
			val key = keys.next()
			val res = setValue(context, key, settings.opt(key))
			if(res.error != null)
				return res.error
		}
		return null
	}

	private fun choice(value: String, label: String) = JSONArray().apply {
		put(value)
		put(label)
	}
}
