// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.setup

import com.nbtapps.store.data.jobs.AutostartSetupState
import com.nbtapps.store.data.jobs.AutostartStep
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.jobs.headUnitId
import com.nbtapps.store.data.link.ProtocolConstants

/** The page the set-up for a new user shows: see [setupStep]. */
sealed interface SetupStep {
    /** What the app needs before it can do anything, and whether the agent is on the car yet. */
    data object Welcome : SetupStep

    /** The agent is not on the car yet: how it is put there, which the app cannot do. */
    data object NotYet : SetupStep

    /** Which of the cars paired with the phone is the one to set up. */
    data object PickCar : SetupStep

    /**
     * A link to the agent on the car is being opened, or [failure] says why none could be. [fromPhone]: the connect
     * only waited for the car's link and none came, so the phone may connect itself: see
     * [ConnectionState.Failed.listenedOnly].
     */
    data class Connect(val failure: String? = null, val fromPhone: Boolean = false) : SetupStep

    /**
     * The agent waits for the code from the car's screen. [stale]: it knew this phone once and no longer does.
     * [offersAutostart]: the agent can be started with the car, so the box that sets that up is offered.
     */
    data class Pair(val stale: Boolean = false, val offersAutostart: Boolean = false) : SetupStep

    /** The phone is paired, and the car is being set up to start the agent with it. */
    data object StartWithCar : SetupStep

    /** The phone is paired, and the set-up is over. */
    data object Done : SetupStep
}

/**
 * What the user has answered and what has been seen so far, as the set-up keeps it in the saved state: see
 * [setupStep].
 */
data class SetupAnswers(
    /** The user said the agent is on the car (true), or that it is not yet (false). Null before either. */
    val agentInstalled: Boolean? = null,
    /** This app's id of the head unit being set up: the car picked. Null before one is. */
    val headUnitId: String? = null,
    /** The agent on [headUnitId] was seen to take this phone: it is paired, now or from before. */
    val paired: Boolean = false,
    /** The user has left the page that shows the car being set up to start the agent. */
    val movedOn: Boolean = false,
    /**
     * Whether the agent on [headUnitId] can be started with the car, as it said while it waited for the code. Null
     * until it has waited for one: the Pair page has not been reached.
     */
    val offersAutostart: Boolean? = null,
    /** The box on the Pair page that has the car set up to start the agent with it. Ticked to begin with. */
    val enableAutostart: Boolean = true,
)

/** The reason the Connect page gives where the app has no link to the car and is opening none. */
internal const val NO_LINK = "The app has no link to it now."

/**
 * The page the set-up shows, from what the user has answered and seen ([answers]), the app's link as it stands
 * ([connection]) and the set-up to start the agent with the car ([setup]). [connecting]: the set-up's own connect
 * to the car is under way. [pairing]: a code is with the agent, and its answer is awaited.
 *
 * Before a car is picked, the answers alone decide: no answer yet is [SetupStep.Welcome], "not yet" is
 * [SetupStep.NotYet], and "it is installed" is [SetupStep.PickCar].
 *
 * Once a car is picked, the link to it decides, until the phone is paired: a link being opened, or the set-up's
 * own connect under way, is [SetupStep.Connect]; one that failed is [SetupStep.Connect] with the reason; one
 * that waits for the code is [SetupStep.Pair]. A link that is up is a paired phone ([SetupAnswers.paired] keeps
 * that), and so is one that turns out to be paired already, by an earlier pairing or one made meanwhile from
 * another app's code: Pair is not shown then.
 *
 * Once the phone is paired the page no longer depends on the link, which restarting iDrive takes down:
 * [SetupStep.StartWithCar] while there is a set-up for this head unit and the user has not moved on from it, and
 * [SetupStep.Done] otherwise, and once iDrive restarts. While a code is with the agent the Pair page stays, so that
 * the set-up the pairing begins is there before the next page is chosen.
 *
 * A link or a set-up that is another head unit's counts as none.
 */
internal fun setupStep(
    answers: SetupAnswers,
    connection: ConnectionState,
    setup: AutostartSetupState?,
    connecting: Boolean = false,
    pairing: Boolean = false,
): SetupStep {
    when (answers.agentInstalled) {
        null -> return SetupStep.Welcome
        false -> return SetupStep.NotYet
        true -> Unit
    }
    val id = answers.headUnitId ?: return SetupStep.PickCar
    val own = connection.takeIf { it.headUnitId == id }
    if (pairing) {
        val waiting = own as? ConnectionState.NeedsPairing
        return SetupStep.Pair(waiting?.stale ?: false, waiting?.offersAutostart ?: (answers.offersAutostart == true))
    }
    if (answers.paired || own is ConnectionState.Connected) {
        val mine = setup?.takeIf { it.headUnitId == id }
        return if (mine != null && !answers.movedOn && mine.step != AutostartStep.Restarting) SetupStep.StartWithCar else SetupStep.Done
    }
    return when (own) {
        is ConnectionState.NeedsPairing -> SetupStep.Pair(own.stale, own.offersAutostart)
        is ConnectionState.Connecting -> SetupStep.Connect()
        is ConnectionState.Failed -> if (connecting) SetupStep.Connect() else SetupStep.Connect(own.reason, own.listenedOnly)
        else -> if (connecting) SetupStep.Connect() else SetupStep.Connect(NO_LINK)
    }
}

/** The agent that waits for the code can be started with the car. */
internal val ConnectionState.NeedsPairing.offersAutostart: Boolean
    get() = ProtocolConstants.CAPABILITY_AUTOSTART in capabilities

/**
 * How many numbered steps the set-up's header counts: picking the car, connecting, pairing and starting with the
 * car, or the first three alone once the Pair page has shown that the agent cannot be started with the car, or
 * while its box is cleared. Before the Pair page has been reached, four.
 */
internal fun setupStepCount(answers: SetupAnswers): Int {
    val offers = answers.offersAutostart ?: return 4
    return if (offers && answers.enableAutostart) 4 else 3
}

/** The number of [step] in the header ("Step 2 of 4"), or null for a page that has no number. */
internal fun setupStepNumber(step: SetupStep): Int? = when (step) {
    SetupStep.PickCar -> 1
    is SetupStep.Connect -> 2
    is SetupStep.Pair -> 3
    SetupStep.StartWithCar -> 4
    SetupStep.Welcome, SetupStep.NotYet, SetupStep.Done -> null
}
