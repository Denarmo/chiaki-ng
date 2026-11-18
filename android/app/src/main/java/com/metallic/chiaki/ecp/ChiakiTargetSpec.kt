// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package com.metallic.chiaki.ecp

import android.content.Context
import android.net.Uri
import android.os.SystemClock
import android.util.Log
import com.metallic.chiaki.R
import com.metallic.chiaki.common.RegisteredHost
import com.metallic.chiaki.common.getDatabase
import com.metallic.chiaki.discovery.DiscoveryManager
import com.metallic.chiaki.discovery.serverMac
import com.metallic.chiaki.lib.CreateError
import com.metallic.chiaki.lib.DiscoveryHost
import com.metallic.chiaki.lib.DiscoveryService
import com.metallic.chiaki.lib.DiscoveryServiceOptions
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.InputStream
import java.net.InetSocketAddress
import java.nio.charset.StandardCharsets

data class TargetSpec(
	val host: String? = null,
	val mac: String? = null,
	val nickname: String? = null,
	val targetHint: String? = null,
	val wakeup: Boolean = true,
	val passcode: String? = null
)

data class ResolvedTarget(
	val hostIp: String,
	val registeredHost: RegisteredHost,
	val passcode: String?
)

class TargetResolutionException(val code: String, message: String) : Exception(message)

object ChiakiTargetSpec
{
	private const val TAG = "ChiakiTargetSpec"
	private const val MAX_FILE_BYTES = 64 * 1024

	fun normalizeMac(raw: String?): String?
	{
		if(raw.isNullOrBlank())
			return null
		val cleaned = raw.trim().replace(":", "").replace("-", "").lowercase()
		if(cleaned.length != 12 || !cleaned.all { it in '0'..'9' || it in 'a'..'f' })
			return null
		return cleaned
	}

	fun load(context: Context, contentUri: Uri?, path: String?): TargetSpec
	{
		val rawText = readTargetText(context, contentUri, path)
		if(rawText != null)
		{
			parseTargetText(rawText)?.let { return it }
			throw TargetResolutionException(
				"content_invalid",
				context.getString(R.string.ecp_error_invalid_target, path ?: contentUri?.toString() ?: "")
			)
		}

		val direct = path?.trim()?.takeIf { it.isNotEmpty() }
			?: contentUri?.takeIf { it.scheme == "chiaki" }?.toString()
			?: throw TargetResolutionException("content_missing", context.getString(R.string.ecp_error_missing_target))

		return parseInlineIdentifier(direct)
			?: throw TargetResolutionException(
				"content_invalid",
				context.getString(R.string.ecp_error_invalid_target, direct)
			)
	}

	private fun readTargetText(context: Context, uri: Uri?, path: String?): String?
	{
		val bytes = when
		{
			uri != null && (uri.scheme == "content" || uri.scheme == "file") -> {
				try
				{
					context.contentResolver.openInputStream(uri)?.use { input ->
						readCappedBytes(input, MAX_FILE_BYTES)
					}
				}
				catch(e: Exception)
				{
					Log.w(TAG, "Failed opening content URI $uri", e)
					null
				}
			}
			!path.isNullOrBlank() -> {
				val file = File(path)
				if(file.isFile && file.length() <= MAX_FILE_BYTES)
					runCatching { file.inputStream().use { readCappedBytes(it, MAX_FILE_BYTES) } }.getOrNull()
				else
					null
			}
			else -> null
		} ?: return null

		return bytes.toString(StandardCharsets.UTF_8).trim()
	}

	private fun readCappedBytes(input: InputStream, limit: Int): ByteArray
	{
		val out = ByteArrayOutputStream()
		val buf = ByteArray(4096)
		var remaining = limit
		while(remaining > 0)
		{
			val read = input.read(buf, 0, minOf(buf.size, remaining))
			if(read <= 0)
				break
			out.write(buf, 0, read)
			remaining -= read
		}
		return out.toByteArray()
	}

	private fun parseTargetText(text: String): TargetSpec?
	{
		if(text.isEmpty())
			return null

		if(text.startsWith("{"))
		{
			val json = runCatching { JSONObject(text) }.getOrNull() ?: return null
			val host = firstNonBlank(json, "host", "ip", "address")
			val mac = firstNonBlank(json, "mac", "server_mac", "serverMac")
			val nickname = firstNonBlank(json, "nickname", "name", "server_nickname", "serverNickname")
			val targetHint = firstNonBlank(json, "target")
			val wakeup = if(json.has("wakeup")) json.optBoolean("wakeup", true) else true
			val passcode = firstNonBlank(json, "passcode", "pin")
			if(host == null && mac == null && nickname == null && targetHint == null)
				return null
			return TargetSpec(host, mac, nickname, targetHint, wakeup, passcode)
		}

		if(text.contains("="))
		{
			var host: String? = null
			var mac: String? = null
			var nickname: String? = null
			var targetHint: String? = null
			var wakeup = true
			var passcode: String? = null
			text.lineSequence().forEach { line ->
				val trimmed = line.trim()
				if(trimmed.isEmpty() || trimmed.startsWith("#") || trimmed.startsWith(";"))
					return@forEach
				val eq = trimmed.indexOf('=')
				if(eq <= 0)
					return@forEach
				val k = trimmed.substring(0, eq).trim().lowercase()
				val v = trimmed.substring(eq + 1).trim().trim('"')
				if(v.isEmpty())
					return@forEach
				when(k)
				{
					"host", "ip", "address" -> host = v
					"mac", "server_mac" -> mac = v
					"nickname", "name", "server_nickname" -> nickname = v
					"target" -> targetHint = v
					"wakeup" -> wakeup = v.equals("true", ignoreCase = true) || v == "1"
					"passcode", "pin" -> passcode = v
				}
			}
			if(host != null || mac != null || nickname != null || targetHint != null)
				return TargetSpec(host, mac, nickname, targetHint, wakeup, passcode)
		}

		return parseInlineIdentifier(text.lineSequence().first().trim())
	}

	private fun parseInlineIdentifier(raw: String): TargetSpec?
	{
		if(raw.isBlank())
			return null
		if(raw.startsWith("chiaki://"))
		{
			val parsed = Uri.parse(raw)
			return TargetSpec(
				host = parsed.host?.takeIf { it.isNotBlank() },
				mac = parsed.getQueryParameter("mac"),
				nickname = parsed.getQueryParameter("nickname"),
				targetHint = parsed.getQueryParameter("target"),
				wakeup = parsed.getQueryParameter("wakeup")?.let { !it.equals("false", true) && it != "0" } ?: true,
				passcode = parsed.getQueryParameter("passcode")
			)
		}
		if(normalizeMac(raw) != null)
			return TargetSpec(mac = raw)
		if(looksLikeHost(raw))
			return TargetSpec(host = raw)
		return TargetSpec(nickname = raw)
	}

	private fun looksLikeHost(value: String): Boolean
	{
		val ipv4 = Regex("^\\d{1,3}(\\.\\d{1,3}){3}$")
		if(ipv4.matches(value))
			return true
		if(value.contains(":") && !value.contains(" "))
			return true
		return false
	}

	private fun firstNonBlank(json: JSONObject, vararg keys: String): String?
	{
		for(k in keys)
		{
			if(json.has(k) && !json.isNull(k))
			{
				val v = json.optString(k, "").trim()
				if(v.isNotEmpty())
					return v
			}
		}
		return null
	}

	fun resolve(
		context: Context,
		spec: TargetSpec,
		isCancelled: () -> Boolean,
		onStatus: (progress: Double, text: String) -> Unit
	): ResolvedTarget
	{
		val db = getDatabase(context)
		val (registeredHosts, manualHosts) = runBlocking {
			Pair(
				db.registeredHostDao().getAll().first(),
				db.manualHostDao().getAll().first()
			)
		}

		val normMac = normalizeMac(spec.mac)
		val matchedManual = spec.host?.let { targetHost ->
			manualHosts.firstOrNull { it.host.equals(targetHost, ignoreCase = true) }
		}

		val registered: RegisteredHost? = when
		{
			normMac != null -> registeredHosts.firstOrNull {
				normalizeMac(it.serverMac.toString()) == normMac
			}
			!spec.nickname.isNullOrBlank() -> registeredHosts.firstOrNull {
				it.serverNickname?.equals(spec.nickname, ignoreCase = true) == true
			}
			matchedManual?.registeredHost != null -> registeredHosts.firstOrNull {
				it.id == matchedManual.registeredHost
			}
			!spec.targetHint.isNullOrBlank() -> {
				val wantPs5 = spec.targetHint.equals("ps5", ignoreCase = true)
				val wantPs4 = spec.targetHint.equals("ps4", ignoreCase = true)
				val filtered = when
				{
					wantPs5 -> registeredHosts.filter { it.target.isPS5 }
					wantPs4 -> registeredHosts.filter { !it.target.isPS5 }
					else -> emptyList()
				}
				if(filtered.size == 1) filtered.first() else filtered.firstOrNull()
			}
			registeredHosts.size == 1 -> registeredHosts.first()
			else -> null
		}

		if(registered == null)
		{
			val label = spec.nickname ?: spec.mac ?: spec.host ?: "PlayStation"
			throw TargetResolutionException(
				"keys_missing",
				context.getString(R.string.ecp_error_unregistered, label)
			)
		}

		val explicitHost = spec.host
			?: manualHosts.firstOrNull { it.registeredHost == registered.id }?.host

		onStatus(0.1, context.getString(R.string.ecp_connecting))

		var latestHosts: List<DiscoveryHost> = emptyList()
		val hostsLock = Object()
		var discoveryService: DiscoveryService? = null

		try
		{
			try
			{
				discoveryService = DiscoveryService(
					DiscoveryServiceOptions(
						DiscoveryManager.HOSTS_MAX,
						DiscoveryManager.DROP_PINGS,
						DiscoveryManager.PING_MS,
						InetSocketAddress("255.255.255.255", DiscoveryManager.PORT)
					)
				) { hosts ->
					synchronized(hostsLock) {
						latestHosts = hosts
						hostsLock.notifyAll()
					}
				}
			}
			catch(e: CreateError)
			{
				Log.w(TAG, "DiscoveryService creation failed: ${e.errorCode}")
			}

			if(discoveryService == null)
			{
				if(explicitHost != null)
					return ResolvedTarget(explicitHost, registered, spec.passcode)
				throw TargetResolutionException(
					"internal",
					"Discovery unavailable and no host IP configured"
				)
			}

			val targetMac = registered.serverMac
			val credential = parseCredential(registered.rpRegistKey)

			fun findMatchingHost(hosts: List<DiscoveryHost>): DiscoveryHost?
			{
				hosts.firstOrNull { it.serverMac == targetMac }?.let { return it }
				if(!explicitHost.isNullOrBlank())
					hosts.firstOrNull { it.hostAddr.equals(explicitHost, ignoreCase = true) }?.let { return it }
				if(!spec.nickname.isNullOrBlank())
					hosts.firstOrNull { it.hostName?.equals(spec.nickname, ignoreCase = true) == true }?.let { return it }
				return null
			}

			val discoverWindowMs = if(explicitHost != null) 1400L else 3500L
			val discoverDeadline = SystemClock.uptimeMillis() + discoverWindowMs
			var found: DiscoveryHost? = null

			while(!isCancelled() && SystemClock.uptimeMillis() < discoverDeadline)
			{
				synchronized(hostsLock) {
					found = findMatchingHost(latestHosts)
					if(found == null)
						hostsLock.wait(200L)
				}
				if(found != null)
					break
			}

			if(isCancelled())
				throw TargetResolutionException("internal", "Launch canceled")

			var current = found
			if(current != null && current.state == DiscoveryHost.State.STANDBY)
			{
				if(!spec.wakeup)
					throw TargetResolutionException("internal", "PlayStation is in Rest Mode")

				val wakeHost = current.hostAddr ?: explicitHost ?: "255.255.255.255"
				val label = current.hostName ?: registered.serverNickname ?: wakeHost
				onStatus(0.25, context.getString(R.string.ecp_waking_up, label))

				val wakeDeadline = SystemClock.uptimeMillis() + 20_000L
				var nextPulseAt = 0L
				while(!isCancelled() && SystemClock.uptimeMillis() < wakeDeadline)
				{
					val now = SystemClock.uptimeMillis()
					if(now >= nextPulseAt && credential != null)
					{
						DiscoveryService.wakeup(discoveryService, wakeHost, credential, registered.target.isPS5)
						if(wakeHost != "255.255.255.255")
							DiscoveryService.wakeup(discoveryService, "255.255.255.255", credential, registered.target.isPS5)
						nextPulseAt = now + 2500L
					}
					val elapsed = 1.0 - ((wakeDeadline - now).coerceAtLeast(0L) / 20_000.0)
					onStatus(0.25 + 0.55 * elapsed, context.getString(R.string.ecp_waking_up, label))

					synchronized(hostsLock) {
						current = findMatchingHost(latestHosts)
						if(current?.state != DiscoveryHost.State.READY)
							hostsLock.wait(250L)
					}
					if(current?.state == DiscoveryHost.State.READY)
						break
				}
			}

			if(isCancelled())
				throw TargetResolutionException("internal", "Launch canceled")

			val readyAddr = current?.takeIf { it.state == DiscoveryHost.State.READY }?.hostAddr
			val finalHost = readyAddr ?: explicitHost ?: current?.hostAddr
				?: throw TargetResolutionException(
					"content_missing",
					"PlayStation did not respond to network discovery"
				)

			onStatus(0.85, context.getString(R.string.ecp_connecting))
			return ResolvedTarget(finalHost, registered, spec.passcode)
		}
		finally
		{
			discoveryService?.dispose()
		}
	}

	private fun parseCredential(registKey: ByteArray): ULong?
	{
		val end = registKey.indexOfFirst { it == 0.toByte() }
		val text = registKey.copyOfRange(0, if(end >= 0) end else registKey.size)
			.toString(StandardCharsets.UTF_8)
			.trim()
		return runCatching { text.toULong(16) }.getOrNull()
	}
}
