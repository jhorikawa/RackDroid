package org.rackdroid

import android.app.Activity

/** Play build: there is no self-updater, by policy.
 *
 * Google Play's Device and Network Abuse policy forbids an app from downloading
 * and installing its own updates — that is the store's job. So this flavor does
 * not merely hide the feature: the code is not here, and neither are INTERNET
 * and REQUEST_INSTALL_PACKAGES (see src/sideload/AndroidManifest.xml). An app
 * that ships a dormant updater still ships the permissions, and reviewers read
 * the manifest.
 *
 * The real implementation lives in src/sideload/. Both expose this same API so
 * MainActivity never has to know which build it is running in. */
object AppUpdates {
	const val SUPPORTED = false

	/** Called once per launch. Nothing to do here. */
	fun applies(context: android.content.Context) = false

	fun onStart(activity: Activity) {}

	/** Never reached: this build never starts an install session. */
	fun onNewIntent(activity: Activity, intent: android.content.Intent?) {}

	/** Never reached: the menu row is hidden when SUPPORTED is false. */
	fun checkNow(activity: Activity) {}
}
