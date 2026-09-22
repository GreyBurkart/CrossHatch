#include <gtest/gtest.h>

#include <vector>

#include <ble/BleRemoteAction.h>
#include <ble/BleRemoteCore.h>

namespace {

using ble_remote::Action;
using ble_remote::BleRemoteCore;
using ble_remote::Event;
using ble_remote::Profile;
using ble_remote::Report;
using ble_remote::ReportKind;
using ble_remote::Slot;
using ble_remote::State;

// Records what the transport would have put on the wire, so the tests can
// assert press/release pairing without any NimBLE involvement.
class RecordingSink final : public ble_remote::ReportSink {
 public:
  void emitPressRelease(const Report& press) override {
    presses.push_back(press);
    Report release = press;
    release.modifiers = 0;
    release.keycode = 0;
    release.usage = 0;
    wire.push_back(press);
    wire.push_back(release);
  }

  std::vector<Report> presses;
  std::vector<Report> wire;
};

// Brings a core to the point where presses are actually sendable. The core
// holds atomics, so it is non-copyable and is filled in place.
void makeReady(BleRemoteCore& core) {
  core.reset();
  core.markAdvertising();
  core.onConnected();
  core.onAuthenticated(true, true);
}

std::vector<Event> drainEvents(BleRemoteCore& core) {
  std::vector<Event> out;
  for (Event e = core.nextEvent(); e != Event::None; e = core.nextEvent()) {
    out.push_back(e);
  }
  return out;
}

// --- Profile to report translation ----------------------------------------

TEST(BleRemoteProfiles, PresentationProfileMapsEverySlotToItsSpecifiedKey) {
  EXPECT_EQ(ble_remote::builtInAction(Profile::Presentation, Slot::Next), Action::ArrowRight);
  EXPECT_EQ(ble_remote::builtInAction(Profile::Presentation, Slot::Previous), Action::ArrowLeft);
  EXPECT_EQ(ble_remote::builtInAction(Profile::Presentation, Slot::Primary), Action::KeyB);
  EXPECT_EQ(ble_remote::builtInAction(Profile::Presentation, Slot::Escape), Action::Escape);
  EXPECT_EQ(ble_remote::builtInAction(Profile::Presentation, Slot::Aux1), Action::PageDown);
  EXPECT_EQ(ble_remote::builtInAction(Profile::Presentation, Slot::Aux2), Action::PageUp);
}

TEST(BleRemoteProfiles, MediaProfileUsesConsumerControlsNotKeyboardKeys) {
  for (const Slot slot : {Slot::Next, Slot::Previous, Slot::Primary, Slot::Aux1, Slot::Aux2}) {
    const Report report = ble_remote::reportFor(ble_remote::builtInAction(Profile::Media, slot));
    EXPECT_EQ(report.kind, ReportKind::Consumer);
    EXPECT_NE(report.usage, 0);
  }
  // Escape is a keyboard key in every profile, including Media.
  const Report escape = ble_remote::reportFor(ble_remote::builtInAction(Profile::Media, Slot::Escape));
  EXPECT_EQ(escape.kind, ReportKind::Keyboard);
  EXPECT_EQ(escape.keycode, 0x29);
}

TEST(BleRemoteProfiles, ArrowKeysTranslateToTheirStandardHidUsageIds) {
  EXPECT_EQ(ble_remote::reportFor(Action::ArrowRight).keycode, 0x4F);
  EXPECT_EQ(ble_remote::reportFor(Action::ArrowLeft).keycode, 0x50);
  EXPECT_EQ(ble_remote::reportFor(Action::ArrowDown).keycode, 0x51);
  EXPECT_EQ(ble_remote::reportFor(Action::ArrowUp).keycode, 0x52);
}

TEST(BleRemoteProfiles, ShiftF5IsTheOnlyActionCarryingAModifier) {
  const Report shiftF5 = ble_remote::reportFor(Action::ShiftF5);
  EXPECT_EQ(shiftF5.keycode, 0x3E);
  EXPECT_EQ(shiftF5.modifiers, 0x02);

  for (uint8_t raw = 0; raw < static_cast<uint8_t>(Action::ACTION_COUNT); ++raw) {
    const auto action = static_cast<Action>(raw);
    if (action == Action::ShiftF5) {
      continue;
    }
    EXPECT_EQ(ble_remote::reportFor(action).modifiers, 0) << "action " << static_cast<int>(raw);
  }
}

TEST(BleRemoteProfiles, EveryKeyboardUsageFitsTheReportMapsLogicalMaximum) {
  // The descriptor declares Logical Maximum 101 (0x65) for the key array, so a
  // usage above that would be undeliverable.
  for (uint8_t raw = 0; raw < static_cast<uint8_t>(Action::ACTION_COUNT); ++raw) {
    const Report report = ble_remote::reportFor(static_cast<Action>(raw));
    if (report.kind == ReportKind::Keyboard) {
      EXPECT_LE(report.keycode, 0x65) << "action " << static_cast<int>(raw);
    }
  }
}

TEST(BleRemoteProfiles, NoneAndOutOfRangeActionsProduceNoReport) {
  EXPECT_TRUE(ble_remote::reportFor(Action::None).isEmpty());
  EXPECT_TRUE(ble_remote::reportFor(Action::ACTION_COUNT).isEmpty());
  EXPECT_TRUE(ble_remote::reportFor(static_cast<Action>(250)).isEmpty());
}

// --- Custom slot validation against the whitelist -------------------------

TEST(BleRemoteCustomSlots, ValidCustomEntriesResolveToTheirAction) {
  const uint8_t slots[ble_remote::SLOT_COUNT] = {
      static_cast<uint8_t>(Action::F5),   static_cast<uint8_t>(Action::ShiftF5),
      static_cast<uint8_t>(Action::Mute), static_cast<uint8_t>(Action::Escape),
      static_cast<uint8_t>(Action::Home), static_cast<uint8_t>(Action::End)};
  EXPECT_EQ(ble_remote::resolveSlot(Profile::Custom, Slot::Next, slots, ble_remote::SLOT_COUNT), Action::F5);
  EXPECT_EQ(ble_remote::resolveSlot(Profile::Custom, Slot::Aux2, slots, ble_remote::SLOT_COUNT), Action::End);
}

TEST(BleRemoteCustomSlots, OutOfWhitelistEntriesResolveToNoneRatherThanBeingClamped) {
  const uint8_t slots[ble_remote::SLOT_COUNT] = {200, 255, static_cast<uint8_t>(Action::ACTION_COUNT), 99, 0, 0};
  for (const Slot slot : {Slot::Next, Slot::Previous, Slot::Primary, Slot::Escape}) {
    EXPECT_EQ(ble_remote::resolveSlot(Profile::Custom, slot, slots, ble_remote::SLOT_COUNT), Action::None);
  }
  EXPECT_FALSE(ble_remote::isValidAction(static_cast<uint8_t>(Action::ACTION_COUNT)));
  EXPECT_TRUE(ble_remote::isValidAction(static_cast<uint8_t>(Action::Mute)));
}

TEST(BleRemoteCustomSlots, BuiltInProfilesIgnoreTheCustomTableEntirely) {
  const uint8_t slots[ble_remote::SLOT_COUNT] = {static_cast<uint8_t>(Action::Mute), 0, 0, 0, 0, 0};
  EXPECT_EQ(ble_remote::resolveSlot(Profile::Presentation, Slot::Next, slots, ble_remote::SLOT_COUNT),
            Action::ArrowRight);
}

TEST(BleRemoteCustomSlots, MissingOrShortCustomTableResolvesToNone) {
  EXPECT_EQ(ble_remote::resolveSlot(Profile::Custom, Slot::Next, nullptr, 0), Action::None);
  const uint8_t shortTable[2] = {static_cast<uint8_t>(Action::F5), static_cast<uint8_t>(Action::F5)};
  EXPECT_EQ(ble_remote::resolveSlot(Profile::Custom, Slot::Aux2, shortTable, 2), Action::None);
}

TEST(BleRemoteCustomSlots, ProfileValidationRejectsOutOfRangeIds) {
  EXPECT_TRUE(ble_remote::isValidProfile(0));
  EXPECT_TRUE(ble_remote::isValidProfile(ble_remote::PROFILE_COUNT - 1));
  EXPECT_FALSE(ble_remote::isValidProfile(ble_remote::PROFILE_COUNT));
  EXPECT_FALSE(ble_remote::isValidProfile(200));
}

// --- Press and release pairing --------------------------------------------

TEST(BleRemoteSend, OneQueuedPressProducesExactlyOnePressAndOneRelease) {
  BleRemoteCore core;
  makeReady(core);
  RecordingSink sink;

  ASSERT_TRUE(core.queueAction(Action::ArrowRight));
  EXPECT_EQ(core.drainActions(sink), 1u);

  ASSERT_EQ(sink.wire.size(), 2u);
  EXPECT_EQ(sink.wire[0].keycode, 0x4F);
  EXPECT_EQ(sink.wire[1].keycode, 0);
  EXPECT_EQ(sink.wire[1].modifiers, 0);
}

TEST(BleRemoteSend, ShiftF5ReleaseClearsTheModifierNotJustTheKey) {
  BleRemoteCore core;
  makeReady(core);
  RecordingSink sink;

  ASSERT_TRUE(core.queueAction(Action::ShiftF5));
  core.drainActions(sink);

  ASSERT_EQ(sink.wire.size(), 2u);
  EXPECT_EQ(sink.wire[0].modifiers, 0x02);
  EXPECT_EQ(sink.wire[1].modifiers, 0);
  EXPECT_EQ(sink.wire[1].keycode, 0);
}

TEST(BleRemoteSend, DrainingTwiceDoesNotResendAnAlreadySentPress) {
  BleRemoteCore core;
  makeReady(core);
  RecordingSink sink;

  ASSERT_TRUE(core.queueAction(Action::ArrowRight));
  EXPECT_EQ(core.drainActions(sink), 1u);
  EXPECT_EQ(core.drainActions(sink), 0u);
  EXPECT_EQ(sink.presses.size(), 1u);
}

TEST(BleRemoteSend, UnmappedActionsAreRefusedBeforeReachingTheQueue) {
  BleRemoteCore core;
  makeReady(core);
  EXPECT_FALSE(core.queueAction(Action::None));
  EXPECT_FALSE(core.queueAction(static_cast<Action>(250)));
  EXPECT_EQ(core.pendingActions(), 0u);
}

// --- Queue saturation ------------------------------------------------------

TEST(BleRemoteQueue, AcceptsExactlyEightPendingActionsThenDrops) {
  BleRemoteCore core;
  makeReady(core);

  for (size_t i = 0; i < ble_remote::ACTION_QUEUE_CAPACITY; ++i) {
    EXPECT_TRUE(core.queueAction(Action::ArrowRight)) << "at " << i;
  }
  EXPECT_EQ(core.pendingActions(), ble_remote::ACTION_QUEUE_CAPACITY);

  EXPECT_FALSE(core.queueAction(Action::ArrowRight));
  EXPECT_FALSE(core.queueAction(Action::ArrowLeft));
  EXPECT_EQ(core.droppedActions(), 2u);
  EXPECT_EQ(core.pendingActions(), ble_remote::ACTION_QUEUE_CAPACITY);
}

TEST(BleRemoteQueue, SaturationDropsTheNewestPressAndKeepsTheOldestInOrder) {
  BleRemoteCore core;
  makeReady(core);
  RecordingSink sink;

  for (size_t i = 0; i < ble_remote::ACTION_QUEUE_CAPACITY; ++i) {
    ASSERT_TRUE(core.queueAction(Action::ArrowRight));
  }
  EXPECT_FALSE(core.queueAction(Action::Mute));

  core.drainActions(sink);
  ASSERT_EQ(sink.presses.size(), ble_remote::ACTION_QUEUE_CAPACITY);
  for (const Report& report : sink.presses) {
    EXPECT_EQ(report.keycode, 0x4F);
    EXPECT_EQ(report.kind, ReportKind::Keyboard);
  }
}

TEST(BleRemoteQueue, DrainingFreesRoomForFurtherPresses) {
  BleRemoteCore core;
  makeReady(core);
  RecordingSink sink;

  for (size_t i = 0; i < ble_remote::ACTION_QUEUE_CAPACITY; ++i) {
    ASSERT_TRUE(core.queueAction(Action::ArrowRight));
  }
  EXPECT_FALSE(core.queueAction(Action::ArrowLeft));

  core.drainActions(sink);
  EXPECT_TRUE(core.queueAction(Action::ArrowLeft));
}

// --- Presses are dropped, never replayed, without a ready host ------------

TEST(BleRemoteSend, PressesAreDroppedWhileMerelyAdvertising) {
  BleRemoteCore core;
  core.reset();
  core.markAdvertising();

  EXPECT_FALSE(core.queueAction(Action::ArrowRight));
  EXPECT_EQ(core.pendingActions(), 0u);
}

TEST(BleRemoteSend, PressesAreDroppedWhileConnectedButNotYetAuthenticated) {
  // Stage 0 measured reports sent before authentication being discarded by the
  // host, so Connected must not be sendable.
  BleRemoteCore core;
  core.reset();
  core.markAdvertising();
  core.onConnected();

  ASSERT_EQ(core.state(), State::Connected);
  EXPECT_FALSE(core.queueAction(Action::ArrowRight));
  EXPECT_EQ(core.pendingActions(), 0u);
}

TEST(BleRemoteSend, PressesDroppedWhileDisconnectedAreNotReplayedOnReconnect) {
  BleRemoteCore core;
  makeReady(core);
  RecordingSink sink;

  core.onDisconnected();
  EXPECT_FALSE(core.queueAction(Action::ArrowRight));

  core.onConnected();
  core.onAuthenticated(true, true);
  EXPECT_EQ(core.drainActions(sink), 0u);
  EXPECT_TRUE(sink.wire.empty());
}

TEST(BleRemoteSend, QueuedPressesAreDiscardedWhenTheHostDisconnectsMidBurst) {
  BleRemoteCore core;
  makeReady(core);
  RecordingSink sink;

  for (size_t i = 0; i < 4; ++i) {
    ASSERT_TRUE(core.queueAction(Action::ArrowRight));
  }
  // The host vanished before the activity got to drain them; they belong to a
  // link that no longer exists.
  core.onDisconnected();

  EXPECT_EQ(core.pendingActions(), 0u);
  EXPECT_EQ(core.drainActions(sink), 0u);
  EXPECT_TRUE(sink.wire.empty());
}

TEST(BleRemoteSend, DrainIsANoOpAfterTeardownEvenWithAReadySink) {
  BleRemoteCore core;
  makeReady(core);
  RecordingSink sink;

  ASSERT_TRUE(core.queueAction(Action::ArrowRight));
  core.markOff();

  EXPECT_EQ(core.drainActions(sink), 0u);
  EXPECT_TRUE(sink.wire.empty());
}

// --- Authentication policy -------------------------------------------------

TEST(BleRemoteAuth, UnencryptedOrUnauthenticatedLinksAreRefusedAndStayUnsendable) {
  BleRemoteCore core;
  core.reset();
  core.markAdvertising();
  core.onConnected();

  EXPECT_FALSE(core.onAuthenticated(false, true));
  EXPECT_NE(core.state(), State::Ready);
  EXPECT_FALSE(core.queueAction(Action::ArrowRight));

  EXPECT_FALSE(core.onAuthenticated(true, false));
  EXPECT_NE(core.state(), State::Ready);
  EXPECT_FALSE(core.queueAction(Action::ArrowRight));
}

TEST(BleRemoteAuth, AuthenticationBeforeConnectStillReachesReady) {
  // Observed on hardware during a bonded reconnect: the two callbacks
  // interleaved and authentication was reported first.
  BleRemoteCore core;
  core.reset();
  core.markAdvertising();

  EXPECT_TRUE(core.onAuthenticated(true, true));
  EXPECT_EQ(core.state(), State::Ready);

  core.onConnected();
  EXPECT_EQ(core.state(), State::Ready) << "a late onConnect must not demote a ready link";
  EXPECT_TRUE(core.queueAction(Action::ArrowRight));
}

TEST(BleRemoteAuth, PasskeyIsExposedWhilePairingAndClearedOnceAuthenticated) {
  BleRemoteCore core;
  core.reset();
  core.markAdvertising();

  EXPECT_EQ(core.passkey(), 0u);
  core.setPasskey(424242);
  EXPECT_EQ(core.passkey(), 424242u);

  core.onConnected();
  core.onAuthenticated(true, true);
  EXPECT_EQ(core.passkey(), 0u);
}

// --- Late callbacks after the activity exits ------------------------------

TEST(BleRemoteLifecycle, LateDisconnectAfterTeardownDoesNotResurrectTheStateMachine) {
  BleRemoteCore core;
  makeReady(core);
  core.markOff();
  ASSERT_EQ(core.state(), State::Off);

  // The NimBLE host task can still deliver this after end() returned.
  core.onDisconnected();

  EXPECT_EQ(core.state(), State::Off);
  EXPECT_FALSE(core.queueAction(Action::ArrowRight));
}

TEST(BleRemoteLifecycle, LateEventsAfterTeardownCannotMakeTheRemoteSendable) {
  BleRemoteCore core;
  makeReady(core);
  RecordingSink sink;
  core.markOff();

  core.onConnected();
  EXPECT_FALSE(core.queueAction(Action::ArrowRight));
  EXPECT_EQ(core.drainActions(sink), 0u);
  EXPECT_TRUE(sink.wire.empty());
}

TEST(BleRemoteLifecycle, ResetClearsPendingActionsEventsAndPasskey) {
  BleRemoteCore core;
  makeReady(core);
  ASSERT_TRUE(core.queueAction(Action::ArrowRight));
  core.setPasskey(999999);

  core.reset();

  EXPECT_EQ(core.state(), State::Off);
  EXPECT_EQ(core.pendingActions(), 0u);
  EXPECT_EQ(core.passkey(), 0u);
  EXPECT_EQ(core.droppedActions(), 0u);
  EXPECT_EQ(core.nextEvent(), Event::None);
}

TEST(BleRemoteLifecycle, BeginAfterEndStartsFromACleanQueue) {
  BleRemoteCore core;
  makeReady(core);
  for (size_t i = 0; i < ble_remote::ACTION_QUEUE_CAPACITY; ++i) {
    ASSERT_TRUE(core.queueAction(Action::ArrowRight));
  }
  EXPECT_FALSE(core.queueAction(Action::ArrowRight));
  ASSERT_GT(core.droppedActions(), 0u);

  core.markOff();
  core.reset();
  core.markAdvertising();
  core.onConnected();
  core.onAuthenticated(true, true);

  EXPECT_EQ(core.pendingActions(), 0u);
  EXPECT_EQ(core.droppedActions(), 0u);
  EXPECT_TRUE(core.queueAction(Action::ArrowRight));
}

// --- Event delivery --------------------------------------------------------

TEST(BleRemoteEvents, LifecycleEventsAreDeliveredInOrderAndOnlyOnce) {
  BleRemoteCore core;
  core.reset();
  core.markAdvertising();
  core.onConnected();
  core.onAuthenticated(true, true);
  core.onDisconnected();

  const std::vector<Event> events = drainEvents(core);
  ASSERT_EQ(events.size(), 4u);
  EXPECT_EQ(events[0], Event::Advertising);
  EXPECT_EQ(events[1], Event::Connected);
  EXPECT_EQ(events[2], Event::Authenticated);
  EXPECT_EQ(events[3], Event::Disconnected);
  EXPECT_EQ(core.nextEvent(), Event::None);
}

TEST(BleRemoteEvents, EventQueueSaturationDropsEventsWithoutCorruptingTheState) {
  BleRemoteCore core;
  core.reset();
  core.markAdvertising();

  // Far more transitions than the queue holds; the state must still be right.
  for (size_t i = 0; i < ble_remote::EVENT_QUEUE_CAPACITY * 4; ++i) {
    core.onConnected();
    core.onAuthenticated(true, true);
    core.onDisconnected();
  }

  const std::vector<Event> events = drainEvents(core);
  EXPECT_LE(events.size(), ble_remote::EVENT_QUEUE_CAPACITY);
  EXPECT_EQ(core.state(), State::Advertising);
}

TEST(BleRemoteEvents, RefusedAuthenticationReportsAFailedEvent) {
  BleRemoteCore core;
  core.reset();
  core.markAdvertising();
  core.onConnected();
  (void)drainEvents(core);

  EXPECT_FALSE(core.onAuthenticated(true, false));
  const std::vector<Event> events = drainEvents(core);
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0], Event::Failed);
}

}  // namespace
