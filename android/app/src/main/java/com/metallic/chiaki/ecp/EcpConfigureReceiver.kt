// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package com.metallic.chiaki.ecp

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log
import io.github.ecp.Ecp
import org.json.JSONObject

class EcpConfigureReceiver : BroadcastReceiver()
{
	companion object
	{
		private const val TAG = "EcpConfigureReceiver"
	}

	override fun onReceive(context: Context, intent: Intent?)
	{
		if(intent?.action != Ecp.ACTION_CONFIGURE)
			return

		val payload = intent.getStringExtra(Ecp.EXTRA_CONFIGURE)
		if(payload.isNullOrBlank())
			return

		val pending = goAsync()
		val appContext = context.applicationContext
		Thread({
			try
			{
				val json = JSONObject(payload)
				val err = EcpSettings.applyConfigure(appContext, json.optJSONObject("settings"))
				if(err != null)
					Log.w(TAG, "configure ignored: $err")
			}
			catch(t: Throwable)
			{
				Log.w(TAG, "configure failed", t)
			}
			finally
			{
				pending.finish()
			}
		}, "chiaki-ecp-configure").start()
	}
}
