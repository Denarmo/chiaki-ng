// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package com.metallic.chiaki.main

import android.app.ActivityOptions
import android.content.Intent
import android.os.Bundle
import android.view.Menu
import android.view.MenuItem
import android.view.View
import android.widget.Toast
import androidx.activity.addCallback
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.Observer
import androidx.lifecycle.ViewModelProvider
import androidx.recyclerview.widget.LinearLayoutManager
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.metallic.chiaki.R
import com.metallic.chiaki.common.*
import com.metallic.chiaki.common.ext.putRevealExtra
import com.metallic.chiaki.common.ext.viewModelFactory
import com.metallic.chiaki.databinding.ActivityMainBinding
import com.metallic.chiaki.lib.ConnectInfo
import com.metallic.chiaki.lib.DiscoveryHost
import com.metallic.chiaki.manualconsole.EditManualConsoleActivity
import com.metallic.chiaki.regist.RegistActivity
import com.metallic.chiaki.settings.SettingsActivity
import com.metallic.chiaki.stream.StreamActivity
import org.json.JSONObject

class MainActivity : AppCompatActivity()
{
	private lateinit var viewModel: MainViewModel

	private lateinit var binding: ActivityMainBinding
	private var discoveryMenuItem: MenuItem? = null
	private var pendingExportJson: String? = null

	private val exportShortcutLauncher = registerForActivityResult(ActivityResultContracts.CreateDocument("application/json")) { uri ->
		val payload = pendingExportJson
		pendingExportJson = null
		if(uri == null || payload == null)
			return@registerForActivityResult
		try {
			contentResolver.openOutputStream(uri, "wt")?.use { out ->
				out.write(payload.toByteArray(Charsets.UTF_8))
			} ?: throw IllegalStateException("Null output stream")
			val label = uri.lastPathSegment?.substringAfterLast('/') ?: "shortcut.chiaki"
			Toast.makeText(this, getString(R.string.export_shortcut_saved, label), Toast.LENGTH_SHORT).show()
		} catch(e: Exception) {
			Toast.makeText(this, R.string.export_shortcut_failed, Toast.LENGTH_LONG).show()
		}
	}

	override fun onCreate(savedInstanceState: Bundle?)
	{
		super.onCreate(savedInstanceState)
		binding = ActivityMainBinding.inflate(layoutInflater)
		setContentView(binding.root)

		title = ""
		setSupportActionBar(binding.toolbar)

		binding.floatingActionButton.setOnClickListener {
			expandFloatingActionButton(!binding.floatingActionButton.isExpanded)
		}
		binding.floatingActionButtonDialBackground.setOnClickListener {
			expandFloatingActionButton(false)
		}

		binding.addManualButton.setOnClickListener { addManualConsole() }
		binding.addManualLabelButton.setOnClickListener { addManualConsole() }

		binding.registerButton.setOnClickListener { showRegistration() }
		binding.registerLabelButton.setOnClickListener { showRegistration() }

		setupBackNavigation()

		viewModel = ViewModelProvider(this, viewModelFactory { MainViewModel(getDatabase(this), Preferences(this)) })
			.get(MainViewModel::class.java)

		val recyclerViewAdapter = DisplayHostRecyclerViewAdapter(this::hostTriggered, this::wakeupHost, this::exportShortcut, this::editHost, this::deleteHost)
		binding.hostsRecyclerView.adapter = recyclerViewAdapter
		binding.hostsRecyclerView.layoutManager = LinearLayoutManager(this)
		viewModel.displayHosts.observe(this, Observer {
			val top = binding.hostsRecyclerView.computeVerticalScrollOffset() == 0
			recyclerViewAdapter.hosts = it
			if(top)
				binding.hostsRecyclerView.scrollToPosition(0)
			updateEmptyInfo()
		})

		viewModel.discoveryActive.observe(this, Observer { active ->
			discoveryMenuItem?.let { updateDiscoveryMenuItem(it, active) }
			updateEmptyInfo()
		})
	}

	private fun updateEmptyInfo()
	{
		if(viewModel.displayHosts.value?.isEmpty() ?: true)
		{
			binding.emptyInfoLayout.visibility = View.VISIBLE
			val discoveryActive = viewModel.discoveryActive.value ?: false
			binding.emptyInfoImageView.setImageResource(if(discoveryActive) R.drawable.ic_discover_on else R.drawable.ic_discover_off)
			binding.emptyInfoTextView.setText(if(discoveryActive) R.string.display_hosts_empty_discovery_on_info else R.string.display_hosts_empty_discovery_off_info)
		}
		else
			binding.emptyInfoLayout.visibility = View.GONE
	}

	private fun expandFloatingActionButton(expand: Boolean)
	{
		binding.floatingActionButton.isExpanded = expand
		binding.floatingActionButton.isActivated = binding.floatingActionButton.isExpanded
	}

	override fun onStart()
	{
		super.onStart()
		viewModel.discoveryManager.resume()
	}

	override fun onStop()
	{
		super.onStop()
		viewModel.discoveryManager.pause()
	}

	private fun setupBackNavigation()
	{
		onBackPressedDispatcher.addCallback(this) {
			if(binding.floatingActionButton.isExpanded)
			{
				expandFloatingActionButton(false)
			}
			else
			{
				isEnabled = false
				onBackPressedDispatcher.onBackPressed()
			}
		}
	}

	override fun onCreateOptionsMenu(menu: Menu): Boolean
	{
		menuInflater.inflate(R.menu.main, menu)
		val discoveryItem = menu.findItem(R.id.action_discover)
		discoveryMenuItem = discoveryItem
		val discoveryActive = viewModel.discoveryActive.value ?: false
		updateDiscoveryMenuItem(discoveryItem, discoveryActive)
		return true
	}

	private fun updateDiscoveryMenuItem(item: MenuItem, active: Boolean)
	{
		item.isChecked = active
		item.setIcon(if(active) R.drawable.ic_discover_on else R.drawable.ic_discover_off)
	}

	override fun onOptionsItemSelected(item: MenuItem): Boolean = when(item.itemId)
	{
		R.id.action_discover ->
		{
			viewModel.discoveryManager.active = !(viewModel.discoveryActive.value ?: false)
			true
		}

		R.id.action_settings ->
		{
			Intent(this, SettingsActivity::class.java).also {
				startActivity(it)
			}
			true
		}

		else -> super.onOptionsItemSelected(item)
	}

	private fun addManualConsole()
	{
		Intent(this, EditManualConsoleActivity::class.java).also {
			it.putRevealExtra(binding.addManualButton, binding.rootLayout)
			startActivity(it, ActivityOptions.makeSceneTransitionAnimation(this).toBundle())
		}
	}

	private fun showRegistration()
	{
		Intent(this, RegistActivity::class.java).also {
			it.putRevealExtra(binding.registerButton, binding.rootLayout)
			startActivity(it, ActivityOptions.makeSceneTransitionAnimation(this).toBundle())
		}
	}

	private fun hostTriggered(host: DisplayHost)
	{
		val registeredHost = host.registeredHost
		if(registeredHost != null)
		{
			fun connect() {
				val connectInfo = ConnectInfo(host.isPS5, host.host, registeredHost.rpRegistKey, registeredHost.rpKey, Preferences(this).videoProfile)
				Intent(this, StreamActivity::class.java).let {
					it.putExtra(StreamActivity.EXTRA_CONNECT_INFO, connectInfo)
					startActivity(it)
				}
			}

			if(host is DiscoveredDisplayHost && host.discoveredHost.state == DiscoveryHost.State.STANDBY)
			{
				MaterialAlertDialogBuilder(this)
					.setMessage(R.string.alert_message_standby_wakeup)
					.setPositiveButton(R.string.action_wakeup) { _, _ ->
						wakeupHost(host)
					}
					.setNeutralButton(R.string.action_connect_immediately) { _, _ ->
						connect()
					}
					.setNegativeButton(R.string.action_connect_cancel_connect) { _, _ -> }
					.create()
					.show()
			}
			else
				connect()
		}
		else
		{
			Intent(this, RegistActivity::class.java).let {
				it.putExtra(RegistActivity.EXTRA_HOST, host.host)
				it.putExtra(RegistActivity.EXTRA_BROADCAST, false)
				if(host is ManualDisplayHost)
					it.putExtra(RegistActivity.EXTRA_ASSIGN_MANUAL_HOST_ID, host.manualHost.id)
				startActivity(it)
			}
		}
	}

	private fun wakeupHost(host: DisplayHost)
	{
		val registeredHost = host.registeredHost ?: return
		viewModel.discoveryManager.sendWakeup(host.host, registeredHost.rpRegistKey, registeredHost.target.isPS5)
	}

	private fun editHost(host: DisplayHost)
	{
		if(host !is ManualDisplayHost)
			return
		Intent(this, EditManualConsoleActivity::class.java).also {
			it.putExtra(EditManualConsoleActivity.EXTRA_MANUAL_HOST_ID, host.manualHost.id)
			startActivity(it)
		}
	}

	private fun deleteHost(host: DisplayHost)
	{
		if(host !is ManualDisplayHost)
			return
		MaterialAlertDialogBuilder(this)
			.setMessage(getString(R.string.alert_message_delete_manual_host, host.manualHost.host))
			.setPositiveButton(R.string.action_delete) { _, _ ->
				viewModel.deleteManualHost(host.manualHost)
			}
			.setNegativeButton(R.string.action_keep) { _, _ -> }
			.create()
			.show()
	}

	private fun exportShortcut(host: DisplayHost)
	{
		val registered = host.registeredHost ?: return
		val nickname = (host.name ?: registered.serverNickname)?.takeIf { it.isNotBlank() }
		val mac = registered.serverMac.toString()
		val targetName = if(registered.target.isPS5) "ps5" else "ps4"
		val json = JSONObject().apply {
			if(nickname != null) put("nickname", nickname)
			put("mac", mac)
			put("target", targetName)
			put("wakeup", true)
			if(host is ManualDisplayHost && host.host.isNotBlank())
				put("host", host.host)
		}
		pendingExportJson = json.toString(2) + "\n"
		val rawBase = nickname ?: mac.replace(":", "")
		val safeBase = rawBase.replace(Regex("[^A-Za-z0-9._ -]+"), "_").trim().ifEmpty { "PlayStation" }
		exportShortcutLauncher.launch("$safeBase.chiaki")
	}
}
