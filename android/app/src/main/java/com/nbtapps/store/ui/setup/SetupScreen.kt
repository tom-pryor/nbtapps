// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.setup

import androidx.activity.compose.BackHandler
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle

/**
 * The set-up for a new user, one page at a time, as [setupStep] decides: whether the agent is on the car, picking
 * the car, connecting, pairing, starting with the car, and Done. Every page can be skipped.
 *
 * [onLeave] leaves the set-up, once it is skipped or over: the caller goes back to where it was opened from.
 * [onBrowse] and [onOpenHeadUnit] are Done's buttons, which leave it for the catalogue or for the head unit's page.
 * Back, the system's or the header's arrow, does what [SetupViewModel.back] says.
 */
@Composable
fun SetupScreen(
    onLeave: () -> Unit,
    onBrowse: () -> Unit,
    onOpenHeadUnit: (String) -> Unit,
    viewModel: SetupViewModel = hiltViewModel(),
) {
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()
    val back = { if (viewModel.back()) onLeave() }
    val skip = {
        viewModel.skip()
        onLeave()
    }
    BackHandler(onBack = back)

    when (val step = uiState.step) {
        SetupStep.Welcome -> WelcomePage(onYes = { viewModel.answer(true) }, onNotYet = { viewModel.answer(false) }, onBrowse = skip)
        SetupStep.NotYet -> NotYetPage(onBack = back, onInstalled = { viewModel.answer(true) })
        SetupStep.PickCar -> PickCarPage(
            uiState,
            onBack = back,
            onSkip = skip,
            onSelect = viewModel::select,
            onConnect = viewModel::pickCar,
            onBeginAddress = viewModel::beginAddress,
            onAdded = viewModel::addedByAddress,
        )
        is SetupStep.Connect -> ConnectPage(
            uiState,
            failure = step.failure,
            onCancel = back,
            onSkip = skip,
            onRetry = viewModel::retry,
            onPickAnother = viewModel::pickAnother,
            onRefresh = viewModel::refreshBluetooth,
        )
        is SetupStep.Pair -> PairPage(
            uiState,
            step,
            onBack = back,
            onSkip = skip,
            onCode = viewModel::setCode,
            onEnableAutostart = viewModel::setEnableAutostart,
            onPair = viewModel::pair,
        )
        else -> SetupFrame(number = setupStepNumber(uiState.step), count = uiState.stepCount, onBack = back, onSkip = skip) {}
    }
}
