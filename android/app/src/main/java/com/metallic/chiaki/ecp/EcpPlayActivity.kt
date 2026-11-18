// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package com.metallic.chiaki.ecp

import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Color
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.text.InputType
import android.util.Log
import android.view.Gravity
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.PixelCopy
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.TextView
import androidx.activity.addCallback
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.metallic.chiaki.R
import com.metallic.chiaki.common.LogManager
import com.metallic.chiaki.common.Preferences
import com.metallic.chiaki.lib.ConnectInfo
import com.metallic.chiaki.lib.ConnectedEvent
import com.metallic.chiaki.lib.CreateError
import com.metallic.chiaki.lib.Event
import com.metallic.chiaki.lib.LoginPinRequestEvent
import com.metallic.chiaki.lib.QuitEvent
import com.metallic.chiaki.lib.Session
import com.metallic.chiaki.session.StreamInput
import com.metallic.chiaki.stream.AspectRatioFrameLayout
import io.github.ecp.Ecp
import io.github.ecp.EcpEngine
import org.json.JSONArray
import org.json.JSONObject
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.math.max

class EcpPlayActivity : AppCompatActivity(), EcpEngine.Callbacks
{
	companion object
	{
		private const val TAG = "EcpPlayActivity"
	}

	private lateinit var aspectRatioLayout: AspectRatioFrameLayout
	private lateinit var surfaceView: SurfaceView
	private lateinit var coverView: View
	private lateinit var level0StatusView: TextView

	private lateinit var preferences: Preferences
	private lateinit var logManager: LogManager
	private lateinit var streamInput: StreamInput

	private var engine: EcpEngine? = null
	private var ecpSession: EcpEngine.Session? = null
	private var chiakiSession: Session? = null
	private var currentVideoAspect: Float = 16f / 9f

	private val mainHandler = Handler(Looper.getMainLooper())
	private var workerExecutor: ExecutorService = Executors.newSingleThreadExecutor { Thread(it, "chiaki-ecp-worker") }

	private val cancelled = AtomicBoolean(false)
	private val shuttingDown = AtomicBoolean(false)
	private val firstFrameEmitted = AtomicBoolean(false)
	private val stopReported = AtomicBoolean(false)

	@Volatile private var pausedByFrontend = false
	@Volatile private var mutedByVolume = false
	@Volatile private var volumePercent = 100
	@Volatile private var resumedAtMs = 0L
	@Volatile private var configuredMenuChordKeys: Set<Int> = emptySet()
	@Volatile private var pendingPasscode: String? = null
	private val heldKeyCodes = mutableSetOf<Int>()
	private var chordTriggered = false

	private var perfPrevFrames = 0L
	private var perfPrevTimeMs = 0L
	private var targetFps = 60.0

	private val frameProbeRunnable = object : Runnable
	{
		override fun run()
		{
			if(shuttingDown.get() || firstFrameEmitted.get())
				return
			val decoded = chiakiSession?.getDecodedFrames() ?: 0L
			if(decoded > 0L && firstFrameEmitted.compareAndSet(false, true))
			{
				coverView.animate().alpha(0f).setDuration(150L).withEndAction {
					coverView.visibility = View.GONE
				}.start()
				level0StatusView.visibility = View.GONE
				engine?.firstFrame()
				engine?.running()
				perfPrevFrames = decoded
				perfPrevTimeMs = SystemClock.uptimeMillis()
				mainHandler.postDelayed(perfRunnable, 1000L)
				return
			}
			mainHandler.postDelayed(this, 50L)
		}
	}

	private val perfRunnable = object : Runnable
	{
		override fun run()
		{
			if(shuttingDown.get())
				return
			if(!pausedByFrontend && firstFrameEmitted.get())
			{
				val now = SystemClock.uptimeMillis()
				val framesNow = chiakiSession?.getDecodedFrames() ?: perfPrevFrames
				val dtMs = (now - perfPrevTimeMs).coerceAtLeast(1L)
				val fps = ((framesNow - perfPrevFrames).coerceAtLeast(0L) * 1000.0) / dtMs.toDouble()
				val speed = if(targetFps > 0.0) fps / targetFps else 1.0
				val frameMs = if(fps > 0.1) 1000.0 / fps else if(targetFps > 0.0) 1000.0 / targetFps else 16.67
				perfPrevFrames = framesNow
				perfPrevTimeMs = now
				engine?.perf(fps, targetFps, speed, frameMs)
			}
			mainHandler.postDelayed(this, 1000L)
		}
	}

	override fun onCreate(savedInstanceState: Bundle?)
	{
		super.onCreate(savedInstanceState)

		preferences = Preferences(this)
		logManager = LogManager(this)
		volumePercent = EcpSettings.getAudioVolume(this)

		buildViews()
		setupBackHandling()

		streamInput = StreamInput(this, preferences).also { input ->
			input.observe(this)
			input.controllerStateChangedCallback = { state ->
				if(!pausedByFrontend)
					chiakiSession?.setControllerState(state)
			}
		}

		startFromIntent(intent)
	}

	override fun onNewIntent(intent: Intent)
	{
		super.onNewIntent(intent)
		setIntent(intent)

		if(EcpEngine.isBringBack(intent, ecpSession))
		{
			EcpEngine.applyFullscreen(this, ecpSession?.fullscreen ?: true)
			if(pausedByFrontend)
			{
				pausedByFrontend = false
				applyAudioState()
				perfPrevFrames = chiakiSession?.getDecodedFrames() ?: 0L
				perfPrevTimeMs = SystemClock.uptimeMillis()
				engine?.resumed()
			}
			return
		}

		resetSessionForNewLaunch()
		startFromIntent(intent)
	}

	override fun onResume()
	{
		super.onResume()
		resumedAtMs = SystemClock.uptimeMillis()
		EcpEngine.applyFullscreen(this, ecpSession?.fullscreen ?: true)
		if(pausedByFrontend)
		{
			pausedByFrontend = false
			applyAudioState()
			perfPrevFrames = chiakiSession?.getDecodedFrames() ?: 0L
			perfPrevTimeMs = SystemClock.uptimeMillis()
			engine?.resumed()
		}
	}

	override fun onPause()
	{
		super.onPause()
		heldKeyCodes.clear()
		chordTriggered = false
		if(::streamInput.isInitialized)
			streamInput.resetState()
	}

	override fun onDestroy()
	{
		shuttingDown.set(true)
		cancelled.set(true)
		mainHandler.removeCallbacks(frameProbeRunnable)
		mainHandler.removeCallbacks(perfRunnable)
		teardownChiakiSession()
		engine?.close()
		engine = null
		workerExecutor.shutdownNow()
		super.onDestroy()
	}

	private fun buildViews()
	{
		val root = FrameLayout(this).apply {
			setBackgroundColor(Color.BLACK)
			keepScreenOn = true
		}

		aspectRatioLayout = AspectRatioFrameLayout(this).apply {
			aspectRatio = currentVideoAspect
		}
		surfaceView = SurfaceView(this).apply {
			layoutParams = FrameLayout.LayoutParams(
				FrameLayout.LayoutParams.MATCH_PARENT,
				FrameLayout.LayoutParams.MATCH_PARENT
			)
			holder.addCallback(object : SurfaceHolder.Callback
			{
				override fun surfaceCreated(holder: SurfaceHolder) {}
				override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int)
				{
					chiakiSession?.setSurface(holder.surface)
				}
				override fun surfaceDestroyed(holder: SurfaceHolder)
				{
					chiakiSession?.setSurface(null)
				}
			})
		}
		aspectRatioLayout.addView(surfaceView)
		root.addView(
			aspectRatioLayout,
			FrameLayout.LayoutParams(
				FrameLayout.LayoutParams.MATCH_PARENT,
				FrameLayout.LayoutParams.MATCH_PARENT,
				Gravity.CENTER
			)
		)

		coverView = View(this).apply {
			setBackgroundColor(Color.BLACK)
			alpha = 1f
		}
		root.addView(
			coverView,
			FrameLayout.LayoutParams(
				FrameLayout.LayoutParams.MATCH_PARENT,
				FrameLayout.LayoutParams.MATCH_PARENT
			)
		)

		level0StatusView = TextView(this).apply {
			setTextColor(Color.WHITE)
			textSize = 16f
			gravity = Gravity.CENTER
			visibility = View.GONE
			setPadding(48, 24, 48, 24)
		}
		root.addView(
			level0StatusView,
			FrameLayout.LayoutParams(
				FrameLayout.LayoutParams.WRAP_CONTENT,
				FrameLayout.LayoutParams.WRAP_CONTENT,
				Gravity.CENTER
			)
		)

		setContentView(root)
	}

	private fun setupBackHandling()
	{
		onBackPressedDispatcher.addCallback(this) {
			if(engine != null && engine?.frontendHas("menu") == true)
			{
				openFrontendMenu()
			}
			else if(engine == null)
			{
				stopRequestedGracefully()
			}
		}
	}

	private fun startFromIntent(intent: Intent)
	{
		val session = EcpEngine.Session.from(intent)
		ecpSession = session
		EcpEngine.applyFullscreen(this, session?.fullscreen ?: true)

		if(session != null)
		{
			val descriptor = EcpEngine.descriptor(this, R.raw.ecp)
			engine = EcpEngine(descriptor, this).also { it.connect(session) }
		}
		else
		{
			runLevel0(intent.data, intent.getStringExtra(Ecp.EXTRA_PATH))
		}
	}

	private fun resetSessionForNewLaunch()
	{
		mainHandler.removeCallbacks(frameProbeRunnable)
		mainHandler.removeCallbacks(perfRunnable)
		cancelled.set(true)
		teardownChiakiSession()
		engine?.close()
		engine = null
		firstFrameEmitted.set(false)
		stopReported.set(false)
		pausedByFrontend = false
		heldKeyCodes.clear()
		chordTriggered = false
		configuredMenuChordKeys = emptySet()
		coverView.visibility = View.VISIBLE
		coverView.alpha = 1f
		cancelled.set(false)
	}

	private fun runLevel0(content: Uri?, path: String?)
	{
		mainHandler.post {
			level0StatusView.text = getString(R.string.ecp_connecting)
			level0StatusView.visibility = View.VISIBLE
		}
		workerExecutor.execute {
			try
			{
				val spec = ChiakiTargetSpec.load(this, content, path)
				val resolved = ChiakiTargetSpec.resolve(
					context = this,
					spec = spec,
					isCancelled = { cancelled.get() },
					onStatus = { _, text ->
						mainHandler.post {
							level0StatusView.text = text
							level0StatusView.visibility = View.VISIBLE
						}
					}
				)
				startChiakiStream(resolved, onStarted = null, onError = { msg ->
					mainHandler.post {
						MaterialAlertDialogBuilder(this)
							.setMessage(msg)
							.setPositiveButton(android.R.string.ok) { _, _ -> stopRequestedGracefully() }
							.setOnCancelListener { stopRequestedGracefully() }
							.show()
					}
				})
			}
			catch(e: TargetResolutionException)
			{
				mainHandler.post {
					MaterialAlertDialogBuilder(this)
						.setMessage(e.message ?: "Failed to resolve PlayStation")
						.setPositiveButton(android.R.string.ok) { _, _ -> stopRequestedGracefully() }
						.setOnCancelListener { stopRequestedGracefully() }
						.show()
				}
			}
		}
	}

	// ---- EcpEngine.Callbacks ----

	override fun handleLaunch(r: EcpEngine.Request)
	{
		val sess = ecpSession
		val rawContent = r.string("content")
		val extraPath = r.string("path") ?: sess?.path

		val (contentUri, path) = when
		{
			rawContent.isNullOrBlank() || rawContent == "intent" -> Pair(sess?.content, extraPath)
			rawContent.startsWith("content://") ||
				rawContent.startsWith("file://") ||
				rawContent.startsWith("chiaki://") -> Pair(Uri.parse(rawContent), extraPath)
			else -> Pair(null, rawContent)
		}

		if(r.msg.has("settings") && !r.msg.isNull("settings"))
		{
			val err = EcpSettings.applyConfigure(this, r.msg.optJSONObject("settings"))
			if(err != null)
			{
				r.error("protocol", err, null)
				return
			}
		}
		volumePercent = EcpSettings.getAudioVolume(this)

		if(r.msg.has("fullscreen"))
		{
			val fs = r.bool("fullscreen", true)
			mainHandler.post { EcpEngine.applyFullscreen(this, fs) }
		}

		configuredMenuChordKeys = parseMenuChordArray(r.msg.optJSONArray("menu_chord"))

		cancelled.set(false)
		stopReported.set(false)
		firstFrameEmitted.set(false)

		try
		{
			val spec = ChiakiTargetSpec.load(this, contentUri, path)
			val resolved = ChiakiTargetSpec.resolve(
				context = this,
				spec = spec,
				isCancelled = { cancelled.get() || shuttingDown.get() },
				onStatus = { progress, text ->
					engine?.loading(progress, text)
				}
			)
			startChiakiStream(
				resolved = resolved,
				onStarted = { r.ok() },
				onError = { msg -> r.error("internal", msg, null) }
			)
		}
		catch(e: TargetResolutionException)
		{
			r.error(e.code, e.message, null)
		}
		catch(e: Exception)
		{
			Log.e(TAG, "Launch error", e)
			r.error("internal", e.message ?: "Unexpected launch error", null)
		}
	}

	override fun handleStop(r: EcpEngine.Request)
	{
		cancelled.set(true)
		shuttingDown.set(true)
		mainHandler.removeCallbacks(frameProbeRunnable)
		mainHandler.removeCallbacks(perfRunnable)
		teardownChiakiSession()
		if(stopReported.compareAndSet(false, true))
			engine?.stopped("frontend")
		r.ok()
		mainHandler.post { finish() }
	}

	override fun handlePause(r: EcpEngine.Request)
	{
		pausedByFrontend = true
		mainHandler.post { streamInput.resetState() }
		applyAudioState()
		engine?.paused("frontend")
		r.ok()
	}

	override fun handleResume(r: EcpEngine.Request)
	{
		pausedByFrontend = false
		applyAudioState()
		perfPrevFrames = chiakiSession?.getDecodedFrames() ?: 0L
		perfPrevTimeMs = SystemClock.uptimeMillis()
		engine?.resumed()
		r.ok()
	}

	override fun handleGetSchema(r: EcpEngine.Request)
	{
		r.ok(JSONObject().put("settings", EcpSettings.getSchema()))
	}

	override fun handleGetSettings(r: EcpEngine.Request)
	{
		r.ok(JSONObject().put("values", EcpSettings.getValues(this)))
	}

	override fun handleSetSetting(r: EcpEngine.Request)
	{
		val key = r.string("key")
		if(key.isNullOrBlank())
		{
			r.error("protocol", "Missing setting key", null)
			return
		}
		val value = if(r.msg.has("value")) r.msg.opt("value") else null
		val res = EcpSettings.setValue(this, key, value)
		if(res.error != null)
		{
			r.error("protocol", res.error, null)
			return
		}
		if(key == EcpSettings.KEY_AUDIO_VOLUME)
		{
			volumePercent = EcpSettings.getAudioVolume(this)
			applyAudioState()
		}
		r.ok(JSONObject().put("applied", res.applied ?: "restart"))
	}

	override fun handleConfigure(r: EcpEngine.Request)
	{
		val err = EcpSettings.applyConfigure(this, r.msg.optJSONObject("settings"))
		if(err != null)
		{
			r.error("protocol", err, null)
			return
		}
		volumePercent = EcpSettings.getAudioVolume(this)
		applyAudioState()
		r.ok()
	}

	override fun handleSetVolume(r: EcpEngine.Request)
	{
		if(r.msg.has("percent") && !r.msg.isNull("percent"))
		{
			val pct = r.double("percent", volumePercent.toDouble())
			if(pct.isNaN() || pct < 0.0 || pct > 100.0)
			{
				r.error("protocol", "percent must be between 0 and 100", null)
				return
			}
			volumePercent = pct.toInt()
			EcpSettings.setAudioVolume(this, volumePercent)
		}
		if(r.msg.has("mute") && !r.msg.isNull("mute"))
			mutedByVolume = r.bool("mute", false)
		applyAudioState()
		r.ok()
	}

	override fun handleScreenshot(r: EcpEngine.Request)
	{
		captureSurfaceBitmap { bitmap ->
			if(bitmap == null)
			{
				r.error("internal", "Surface unavailable for screenshot", null)
				return@captureSurfaceBitmap
			}
			try
			{
				r.ok(JSONObject().put("image", Ecp.picture(bitmap)))
			}
			finally
			{
				bitmap.recycle()
			}
		}
	}

	override fun onClosed(reason: String?)
	{
		Log.i(TAG, "ECP channel closed: $reason")
		cancelled.set(true)
		shuttingDown.set(true)
		mainHandler.removeCallbacks(frameProbeRunnable)
		mainHandler.removeCallbacks(perfRunnable)
		teardownChiakiSession()
		mainHandler.post {
			if(!isFinishing)
				finish()
		}
	}

	// ---- Chiaki Session management ----

	private fun startChiakiStream(
		resolved: ResolvedTarget,
		onStarted: (() -> Unit)?,
		onError: (String) -> Unit
	)
	{
		val videoProfile = preferences.videoProfile
		targetFps = videoProfile.maxFPS.toDouble()
		currentVideoAspect = if(videoProfile.height > 0)
			videoProfile.width.toFloat() / videoProfile.height.toFloat()
		else
			16f / 9f
		pendingPasscode = resolved.passcode

		val connectInfo = ConnectInfo(
			ps5 = resolved.registeredHost.target.isPS5,
			host = resolved.hostIp,
			registKey = resolved.registeredHost.rpRegistKey,
			morning = resolved.registeredHost.rpKey,
			videoProfile = videoProfile
		)

		val created = try
		{
			Session(connectInfo, logManager.createNewFile().file.absolutePath, preferences.logVerbose)
		}
		catch(e: CreateError)
		{
			onError("Failed to create session: ${e.errorCode}")
			return
		}

		created.eventCallback = { event -> handleChiakiEvent(event) }
		val startErr = created.start()
		if(!startErr.isSuccess)
		{
			created.dispose()
			onError("Failed to start session: $startErr")
			return
		}

		chiakiSession = created
		applyAudioState()

		mainHandler.post {
			aspectRatioLayout.aspectRatio = currentVideoAspect
			val surface = surfaceView.holder.surface
			if(surface != null && surface.isValid)
				created.setSurface(surface)
			mainHandler.removeCallbacks(frameProbeRunnable)
			mainHandler.postDelayed(frameProbeRunnable, 50L)
		}

		onStarted?.invoke()
	}

	private fun handleChiakiEvent(event: Event)
	{
		when(event)
		{
			is ConnectedEvent -> {
				engine?.loading(0.95, getString(R.string.ecp_connecting))
			}
			is LoginPinRequestEvent -> {
				val code = pendingPasscode
				pendingPasscode = null
				if(!code.isNullOrBlank())
				{
					chiakiSession?.setLoginPin(code)
				}
				else
				{
					mainHandler.post { showPinDialog(event.pinIncorrect) }
				}
			}
			is QuitEvent -> {
				Log.i(TAG, "Chiaki quit: ${event.reason} (${event.reasonString})")
				if(!shuttingDown.getAndSet(true))
				{
					mainHandler.removeCallbacks(frameProbeRunnable)
					mainHandler.removeCallbacks(perfRunnable)
					teardownChiakiSession()
					if(stopReported.compareAndSet(false, true))
					{
						if(event.reason.isError)
						{
							val msg = event.reasonString ?: event.reason.toString()
							engine?.stopped("error", "internal", msg, null)
						}
						else
						{
							engine?.stopped("user")
						}
					}
					mainHandler.post {
						if(!isFinishing)
							finish()
					}
				}
			}
			else -> {}
		}
	}

	private fun showPinDialog(incorrect: Boolean)
	{
		if(isFinishing || isDestroyed)
			return
		val input = EditText(this).apply {
			inputType = InputType.TYPE_CLASS_NUMBER
		}
		MaterialAlertDialogBuilder(this)
			.setTitle(R.string.alert_message_login_pin_request)
			.setMessage(if(incorrect) R.string.alert_message_login_pin_request_incorrect else R.string.alert_message_login_pin_request)
			.setView(input)
			.setPositiveButton(android.R.string.ok) { _, _ ->
				chiakiSession?.setLoginPin(input.text.toString())
			}
			.setNegativeButton(android.R.string.cancel) { _, _ ->
				stopRequestedGracefully()
			}
			.setOnCancelListener {
				stopRequestedGracefully()
			}
			.show()
	}

	private fun teardownChiakiSession()
	{
		val s = chiakiSession ?: return
		chiakiSession = null
		runCatching {
			s.setSurface(null)
			s.stop()
			s.dispose()
		}
	}

	private fun applyAudioState()
	{
		val s = chiakiSession ?: return
		s.setVolume((volumePercent / 100f).coerceIn(0f, 1f))
		s.setMuted(mutedByVolume || pausedByFrontend)
	}

	private fun stopRequestedGracefully()
	{
		cancelled.set(true)
		shuttingDown.set(true)
		mainHandler.removeCallbacks(frameProbeRunnable)
		mainHandler.removeCallbacks(perfRunnable)
		teardownChiakiSession()
		if(stopReported.compareAndSet(false, true))
			engine?.stopped("user")
		finish()
	}

	// ---- Menu chord & Input ----

	override fun dispatchKeyEvent(event: KeyEvent): Boolean
	{
		if(event.downTime < resumedAtMs)
			return true

		if(event.keyCode == KeyEvent.KEYCODE_BACK)
		{
			if(event.action == KeyEvent.ACTION_UP && !event.isCanceled)
				onBackPressedDispatcher.onBackPressed()
			return true
		}

		val chord = configuredMenuChordKeys
		if(chord.isNotEmpty() && engine?.frontendHas("menu") == true)
		{
			when(event.action)
			{
				KeyEvent.ACTION_DOWN -> {
					heldKeyCodes.add(event.keyCode)
					if(!chordTriggered && heldKeyCodes.containsAll(chord))
					{
						chordTriggered = true
						openFrontendMenu()
						return true
					}
				}
				KeyEvent.ACTION_UP -> {
					heldKeyCodes.remove(event.keyCode)
					if(!heldKeyCodes.containsAll(chord))
						chordTriggered = false
				}
			}
		}

		if(!pausedByFrontend && streamInput.dispatchKeyEvent(event))
			return true
		return super.dispatchKeyEvent(event)
	}

	override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean
	{
		if(!pausedByFrontend && streamInput.onGenericMotionEvent(event))
			return true
		return super.dispatchGenericMotionEvent(event)
	}

	private fun openFrontendMenu()
	{
		val eng = engine ?: return
		if(!eng.frontendHas("menu") || pausedByFrontend)
			return
		pausedByFrontend = true
		streamInput.resetState()
		applyAudioState()

		captureSurfaceBitmap { captured ->
			val frame = captured ?: Bitmap.createBitmap(16, 16, Bitmap.Config.ARGB_8888).apply {
				eraseColor(Color.BLACK)
			}
			val recycleFallback = captured == null
			eng.requestMenu(this, frame)
			if(recycleFallback || captured != null)
				frame.recycle()
		}
	}

	private fun captureSurfaceBitmap(callback: (Bitmap?) -> Unit)
	{
		mainHandler.post {
			val sv = surfaceView
			val surface = sv.holder.surface
			val w = sv.width
			val h = sv.height
			if(surface == null || !surface.isValid || w <= 0 || h <= 0)
			{
				callback(null)
				return@post
			}
			val maxDim = max(w, h)
			val scale = if(maxDim > 1280) 1280f / maxDim.toFloat() else 1f
			val bw = max(1, (w * scale).toInt())
			val bh = max(1, (h * scale).toInt())
			val bitmap = Bitmap.createBitmap(bw, bh, Bitmap.Config.ARGB_8888)
			try
			{
				PixelCopy.request(sv, bitmap, { copyResult ->
					if(copyResult == PixelCopy.SUCCESS)
					{
						callback(bitmap)
					}
					else
					{
						bitmap.recycle()
						callback(null)
					}
				}, mainHandler)
			}
			catch(e: IllegalArgumentException)
			{
				Log.w(TAG, "PixelCopy unavailable", e)
				bitmap.recycle()
				callback(null)
			}
		}
	}

	private fun parseMenuChordArray(arr: JSONArray?): Set<Int>
	{
		if(arr == null || arr.length() == 0)
			return emptySet()
		val out = mutableSetOf<Int>()
		for(i in 0 until arr.length())
		{
			val tok = arr.optString(i, "").trim().lowercase()
			val code = when(tok)
			{
				"a" -> KeyEvent.KEYCODE_BUTTON_A
				"b" -> KeyEvent.KEYCODE_BUTTON_B
				"x" -> KeyEvent.KEYCODE_BUTTON_X
				"y" -> KeyEvent.KEYCODE_BUTTON_Y
				"l" -> KeyEvent.KEYCODE_BUTTON_L1
				"r" -> KeyEvent.KEYCODE_BUTTON_R1
				"l2" -> KeyEvent.KEYCODE_BUTTON_L2
				"r2" -> KeyEvent.KEYCODE_BUTTON_R2
				"l3" -> KeyEvent.KEYCODE_BUTTON_THUMBL
				"r3" -> KeyEvent.KEYCODE_BUTTON_THUMBR
				"select" -> KeyEvent.KEYCODE_BUTTON_SELECT
				"start" -> KeyEvent.KEYCODE_BUTTON_START
				"up" -> KeyEvent.KEYCODE_DPAD_UP
				"down" -> KeyEvent.KEYCODE_DPAD_DOWN
				"left" -> KeyEvent.KEYCODE_DPAD_LEFT
				"right" -> KeyEvent.KEYCODE_DPAD_RIGHT
				else -> null
			} ?: return emptySet()
			out.add(code)
		}
		return out
	}
}
