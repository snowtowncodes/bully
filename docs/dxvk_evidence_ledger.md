# DXVK Evidence Ledger

Scope: M0 documentation artifact. This ledger records retained repository
evidence and history; it does not report a new runtime probe. The source
checkpoint is `da5831d` (`feat(mods): add Present-hook backbuffer marker`).

## Architecture And Evidence Boundary

Bully dynamically loads `D3D9.DLL`. The executable-directory `d3d9.dll`
proxy selects a native D3D9, DXVK, or experimental D3D9On12 backend and keeps
the D3D9 wrapper/telemetry boundary active. For DXVK, the proxy loads the
sibling `dxvk_d3d9.dll`; DXVK then provides the D3D9-to-Vulkan implementation.
The architecture is described in [architecture.md](architecture.md).

The evidence boundary is deliberately narrower than successful API calls:

- `report.json` is the authoritative record for a probe run.
- A proxy log can establish requested/effective backend, device creation,
  wrapper activity, and `Present` HRESULTs. A DXVK log can establish DXVK and
  Vulkan device/swapchain activity. Neither log alone proves visible output.
- A pre-Present backbuffer BMP establishes that rendering reached that
  backbuffer. It does not establish that the game image reached the display.
- Visible-output proof requires a selected, uncontaminated, nonblank capture
  whose target is `main-window`, together with the applicable backend evidence.
  A desktop/window capture that includes another visible application is
  rejected for that purpose.

Status terms used below:

- **Observed**: stated directly by a retained report, log, capture, or commit.
- **Inferred**: a bounded conclusion supported by observed evidence; it is not
  a direct visual observation.
- **Unresolved**: the available evidence does not identify the cause or prove
  the claim.
- **Not yet tested**: the evidence required for the claim is absent from this
  ledger; this is not a failure result.

## Recorded Evidence

### Historical Visible DXVK Proof

**Observed** --
`dump/render-probe/20260814-175037-pid44752-dxvk-se-none_pi-none_od-i/`
is the retained historical proxy-DXVK visual proof. Its report records
`backend=dxvk`, passed display preflight, an installed-and-restored probe run,
and selected nonblank `main-window` captures at 5 and 30 seconds. The proxy
artifact records the DXVK module load, symbol resolution, successful D3D9
creation, wrapped interfaces, and successful `Present` calls. The associated
DXVK evidence records DXVK 3.0.2 using Vulkan. The chainload manifest records
successful restoration.

**Inferred** --
For that archived configuration, Bully was visibly rendered through the
proxy-DXVK path. This is historical evidence only; it does not establish that
every later build, configuration, or run presents identically.

### Later Direct And Proxy DXVK Observations

**Observed** --
The later direct-DXVK helper record
`dump/render-probe/dxvk-20260814-210513/` points to child run
`20260814-210518-pid32528-native-se-none_pi-none_od-i`. That child run used
`-NoInstall`, recorded all selected `main-window` captures as blank-white,
and had no updated proxy backbuffer artifact. Its direct-DXVK log records
DXVK 3.0.2 and Vulkan device/swapchain setup. Because the proxy was not the
active capture path, this direct observation supplies no proxy-backbuffer
evidence.

**Observed** --
Later proxy-DXVK runs, including
`20260814-203446-pid2684-dxvk-se-none_pi-none_od-i` and
`20260814-210152-pid4832-dxvk-se-none_pi-none_od-i`, recorded selected
`main-window` captures as blank-white while their proxy artifacts recorded
nonblank pre-Present backbuffers. The `20260814-203446` proxy log also records
DXVK loading, successful device creation and `Present`, and non-uniform
backbuffer luminance data.

**Inferred** --
For the proxy observations, rendering reached the pre-Present backbuffer and
the proxy observed successful presentation calls, but those runs did not
produce qualifying visible output. The direct and proxy records show a later
white-window condition; they do not, by themselves, identify a common cause.

**Unresolved** --
Why the later DXVK runs produced white selected window captures after the
historical visible run is not established. No conclusion should attribute the
condition to DXVK, the proxy, the window/capture path, or a particular source
change without matched evidence.

### Desktop-Capture Contamination And Native Marker

**Observed** --
The native marker run
`dump/render-probe/20260814-211207-pid31140-native-se-none_pi-none_od-i/`
logged `test marker applied` with `ColorFill` success. Its in-process native
backbuffer visibly contains the magenta marker over the Bully title screen.
This proves the marker was applied to that native backbuffer at the existing
`Present` hook.

**Observed** --
The active-window PNGs from that marker run were rejected as visible-marker
proof because the desktop capture included a PowerShell terminal. The
nonblank classification therefore cannot be attributed solely to Bully or to
the marker. This is known desktop-capture contamination, not evidence that the
marker was visibly presented.

**Not yet tested** --
A qualifying visible-marker proof requires the successful marker log and
backbuffer artifact plus an uncontaminated selected `main-window` image that
shows the marker. That combination is not retained here.

### D3D9On12

**Observed** --
`dump/render-probe/20260814-095119-pid17544-on12-se-none_pi-none_od-i/`
recorded a D3D9On12 device verified through `IDirect3DDevice9On12` and an
`ID3D12Device`, successful `Present`, a nonblank pre-Present backbuffer, and
blank-white selected `main-window` captures at all three scheduled times.

**Inferred** --
That tested On12 configuration reached the presentation boundary but did not
yield qualifying visible output.

**Unresolved** --
The presentation failure cause and an On12 configuration that visibly renders
Bully remain unresolved. D3D9On12 is therefore parked; the existing history
does not justify a custom 9On12 fork, presentation-matrix expansion, or game
patches without a new compatibility lead.

## Claims, Evidence, And Gates

| Gate / claim | Sufficient evidence | Ledger state |
| --- | --- | --- |
| M0: renderer architecture and evidence boundary are documented | Dynamic D3D9 load/proxy architecture, backend roles, and capture limits recorded without treating code or logs as visual proof | **Observed** in repository architecture and this ledger |
| M1: the proxy is in the D3D9 path | Proxy load plus `Direct3DCreate9`/`CreateDevice` records and wrapper or `Present` telemetry for the effective backend | **Observed** historically; the M1 proxy milestone is recorded in commit `ce861c0` |
| Proxy-DXVK backend execution | DXVK module/symbol/creation evidence, proxy wrappers, and successful `Present` records | **Observed** for the historical and later proxy-DXVK runs above |
| Visible DXVK output | Backend evidence plus a selected, uncontaminated, nonblank `main-window` capture | **Observed** historically at `20260814-175037-pid44752-dxvk-se-none_pi-none_od-i`; later behavior is **Unresolved** |
| Rendering reached a backbuffer | Pre-Present backbuffer artifact that is nonblank/non-uniform, with matching proxy evidence | **Observed** for the noted proxy-DXVK, native-marker, and On12 cases; it is not a visible-output gate |
| Visible native marker | Successful marker log, marker in backbuffer, and uncontaminated selected `main-window` image containing the marker | **Not yet tested**; the retained active-window image is contaminated |
| Visible D3D9On12 output | Verified On12/D3D12 identity, successful `Present`, nonblank backbuffer, and uncontaminated selected nonblank `main-window` capture | **Unresolved**; the retained matched run has blank-white window captures |

The M0 and M1 gates above describe evidence classification and proxy-path
establishment. They do not relax the visual gate defined in
[m2_test_plan.md](m2_test_plan.md): a backbuffer, frontbuffer, API success, or
contaminated desktop capture cannot substitute for qualifying visible output.
