# Experimental compatibility tracker

Last reviewed: 2026-10-23. Deltagram is experimental and is not yet a complete
Delta Chat client. Core provides messaging protocols and encryption; retaining
a Telegram widget does not establish that its action works with Core.

The feature categories below use the
[Delta Chat platform feature overview, interface issue #39](https://github.com/deltachat/interface/issues/39)
as a reference. That issue compares official Delta Chat clients and is an
incomplete overview, not a Deltagram test report or a roadmap. Its checkmarks
are not copied here. Deltagram evidence comes from [PROGRESS.md](PROGRESS.md),
[VALIDATION.md](VALIDATION.md) and the local Core bridge implementation.

## Status meanings

- **✅ Locally checked:** an isolated Core or GUI check is recorded. This does
  not establish delivery between real accounts or cross-client interoperability.
- **🟡 Partial:** some behavior is implemented, with missing pieces or checks.
- **❌ Unavailable:** explicitly disabled or recorded as unfinished.
- **❓ Unverified:** no adequate Deltagram compatibility check is recorded.

## What works in local checks

| Capability | Status | Evidence and limits |
| --- | --- | --- |
| Core account creation and chat-list loading | ✅ Locally checked | Qt/Core RPC self-test creates a disposable account and loads its list without mail IO. |
| Contacts and contact deletion | ✅ Locally checked | Search, profile opening, chat creation and confirmed deletion have isolated checks. |
| Shared contact cards | ✅ Locally checked | Send/import, card photo and profile navigation checked. Core does not import vCard NOTE biographies; available Core contact status is displayed. |
| Group creation | ✅ Locally checked | Name/photo and member selection persist through Core; selection chips and stored members checked. |
| Invitation links | 🟡 Partial | Contact/group/channel previews, confirmation and navigation checked locally. SecureJoin completed over real mail IO in the Core API check; the complete GUI join flow remains unverified. |
| Channel profiles | ✅ Locally checked | Adaptive page/modal navigation, centered cover, large photo preview, contained action menu, description links, photo/name/description updates, mute control and Core media counts. Media rows open the selected message. |
| Broadcast channels | ✅ Locally checked | Creation and subscriber write restrictions checked. Owner counts are Core read receipts, not public view totals. |
| Message information | ✅ Locally checked | Context action and Core-backed details checked. |
| Reactions | ✅ Locally checked | Picker, stored reactions and rendering checked locally; multi-device delivery remains unverified. |
| Profile sidebar | ✅ Locally checked | Optional experimental toggle replaces the folder strip with profile photos/names. Switching, active colors, immediate toggling and restart persistence checked locally. |
| Profile ordering | ✅ Locally checked | Core RPC ordering and UI feedback recorded. |

## Partial implementation and known gaps

| Capability | Status | Remaining work or validation |
| --- | --- | --- |
| Text, images, files, videos and forwarding | 🟡 Partial | Live Core API checks passed bidirectional text and byte-identical file delivery against Delta Chat 2.62.0. MP4 sending, GUI forwarding and local playback have earlier local checks. Rebuilt GUI delivery, download and forwarding remain to be verified. |
| Delivery and read indicators | 🟡 Partial | A live cross-client Core read receipt passed. Pending, delivered and read rendering have earlier offline fixture checks; rebuilt GUI indicators and multi-recipient behavior need validation. |
| Quotes and replies | 🟡 Partial | A live Core reply retained its quote across clients. Complete GUI create/display/jump behavior remains unverified. |
| Message editing, deletion and chat archiving | 🟡 Partial | Live Core edits retained message IDs and remote deletion passed. The released GUI reproduced optimistic deletion after Core rejected it; the fix and two-client GUI behavior still need verification. |
| Voice messages | 🟡 Partial | Native waveform, duration, play/pause and seeking are implemented; local attachment playback is verified. Microphone recording and live cross-client delivery still need broader testing. |
| Multiple profiles | 🟡 Partial | Switching clears reused histories, drafts, pending messages and attachment caches. Core draft storage and asynchronous callbacks are scoped to the account. End-to-end switching with overlapping chat/message IDs still needs GUI validation. |
| Profile and group photos | 🟡 Partial | Settings image selection/crop/save and account avatar colors checked locally with Core; group checks recorded. Complete camera workflows and live photo distribution need checking. |
| Onboarding and relay settings | 🟡 Partial | Core transport configuration and backup restore paths exist; full live onboarding needs validation. |
| Invite QR scanning | 🟡 Partial | Camera capture, QR image files and clipboard screenshots feed Core invites for an explicit Join. Image decoding is checked locally; physical camera capture needs hardware validation. |
| Second-device setup | 🟡 Partial | Real Core backup QR generation exists; completed transfer to another device is unverified. |
| Contact status and biographies | 🟡 Partial | Available Core contact status displayed; imported vCard biographies are unsupported by the current Core snapshot. |
| Search | 🟡 Partial | Core message search is wired into the bridge; global/in-chat result navigation and help/HTML searches need separate checks. |
| App picker and webxdc | 🟡 Partial | Embedded app store exists; host rendering, draft preparation and synchronized webxdc updates are unverified. |
| Calls | ❌ Unavailable | Delta Chat calling is not implemented. Chat-header call buttons are hidden, call entry points stop before device permissions, Telegram conference creation/joining is disabled, and tde2e is excluded. |
| Service-event presentation | ❌ Unavailable | Recorded as unfinished in the development log. |
| Settings privacy presentation | 🟡 Partial | Hiding the settings email remains unfinished. |

## Compatibility checks still needed

No complete Core-backed Deltagram checks are recorded for these categories.
They are **unverified**, rather than assumed to work because the donor UI has them:

| Feature area | Status | Checks to record |
| --- | --- | --- |
| Media browser | ❓ Unverified | Combined media list, previous/next, delete and return to message. |
| Chat management | ❓ Unverified | Clear history, bulk selection, bulk read state and last-read positioning. |
| Member profiles | ❓ Unverified | Opening a profile from a group member list. |
| Attachments | ❓ Unverified | Staging, multiple files/images/videos and image editing before send. |
| Notifications | ❓ Unverified | Background delivery and unread indicators for inactive profiles. |
| Accessibility and localization | ❓ Unverified | Screen readers, keyboard navigation and RTL layouts. |
| Appearance | 🟡 Partial | General wallpaper: local gallery and custom images in Settings → Chat Settings. Custom-image persistence checked across chats, profiles and restart; per-chat wallpaper is unavailable and removed from the chat menu. Full day/night combinations remain unverified. |
| External integration | ❓ Unverified | Camera capture and sharing into/out of the application. |
| Email presentation | ❓ Unverified | Mailing lists, sender identity and HTML mail display/search. |
| Message content | ❓ Unverified | Stickers, copying links and complete media rendering. |
| Trust information | ❓ Unverified | Core-backed verification details and “verified by” presentation. |
| Group duplication | ❓ Unverified | Cloning a group with correct Core membership semantics. |

## Client integrity regressions (2026-10-23)

The standalone `delta_integrity_tests` target exercises the production Qt RPC
transport, account callback guard, Core draft adapter and history pagination.
Fourteen cases pass in Ubuntu 24.04 x86-64 with Qt 6.4.2: reordered replies,
event delivery, shutdown, failed startup, subprocess exit, malformed responses,
timeouts without retrying mutations, restart framing, account isolation,
draft attachment preservation, draft deletion, serialized draft writes, visible
write failures, and Core-ordered pagination with non-monotonic IDs.
`nix/test-rpc.sh` runs this suite before its real-Core smoke test.
An additional real-Core case passes against Core 2.62.0 and pinned 2.63.0-dev: two unconfigured
accounts with identical chat IDs retain independent drafts; changing draft
text preserves the attachment bytes, and clearing the other draft persists.
Set `DELTA_TEST_CORE_RPC` to run this case directly. `nix/test-rpc.sh` supplies
the pinned server path so this case is required there.

The suite now has 23 cases, passing sequentially against both Core versions.
Added coverage includes acknowledged history/chat deletion, partial failures,
account cancellation, timestamp-filtered deletion in batches, concurrent new
messages, real-Core history deletion, and drafts appearing in chat-list previews
but not in history. Clear-history UI paths now wait for Core confirmation.
Draft preview IDs no longer become history anchors or sent-message bubbles.
Native attachment cache IDs include the account identity. A boundary/collision
regression covers overlapping message IDs; profile changes also clear playback
state and close the media viewer. Rendered cross-profile media checks remain.

The GUI changes also wait for Core before removing messages, apply incoming
text edits, retain actual group sender identity, use Core unread state, guard
callbacks for closed dialogs, and stop the event loop when Core exits.
Forwarding uses Core message events instead of guessing the created IDs.
Profile changes also cancel queued donor history/read requests without resuming
their old generators. A failed or raced unread lookup retains a valid history
anchor instead of inventing message ID 1.
Messages still awaiting a Core send ID cannot be deleted locally; deletion
becomes available after the send completes, avoiding apparent cancellation
followed by the accepted message reappearing.
These paths require full-client regression checks; the standalone tests do not
establish GUI behavior or live interoperability.

Live disposable-account checks passed between pinned Core 2.63.0-dev and the
Core bundled with official Delta Chat Desktop 2.62.0: SecureJoin completion,
bidirectional text, read receipts, edits preserving message IDs, quoted replies,
remote deletion, group sender identity and byte-identical file delivery.
These checks used the RPC APIs; fixed Deltagram GUI behavior is a separate gate.

Existing local draft files lack an account identity and are no longer loaded
by the Delta client. They are retained on disk; new drafts persist through Core.

The first full remote GUI build passed at `4ddcae4`. Its GUI displayed the
actual group sender and restored the alternate profile's own draft. Later
history/deletion changes still await a full build and rendered verification.
That GUI also passed rejected deletion, correct sender-profile navigation,
bidirectional group text, a read receipt, incoming text edits and a quoted reply
against the official desktop GUI. Live checks exposed stale edited previews,
ordinary files rendered as captions only, and missing standalone quote text;
the follow-up presentation fixes await the final build and GUI validation.

Before release, build the final revision and exercise two disposable profiles
with overlapping IDs: switch with an unsent draft and delayed edit, reject a
deletion, receive an edit, open a group sender, paginate non-monotonic message
IDs, close Connectivity while loading, and kill Core with requests pending.
Then verify send/receive, edits, deletions and read receipts against another
Delta Chat client using explicitly authorized test accounts.

## Updating this tracker

Record the source revision, client versions, environment and test scope before
promoting a feature. Distinguish disposable local checks from tests with real
mail IO and another Delta Chat client. Include failure cases and account-switch
behavior. Update the README summary when a known gap changes.

`make test` checks source hygiene and offline regressions; it is not a full
compatibility suite. The Qt/Core smoke test is documented in
[DEVELOPMENT.md](DEVELOPMENT.md). The complete GUI release now links with the
locked toolchain. Portable runtime and distribution package checks are recorded
in [VALIDATION.md](VALIDATION.md). Successful builds and startup checks do not
establish live messaging or complete feature compatibility.
