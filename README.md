# XPBD Simulation

A simulation for XPBD: Position-Based Simulation of Compliant Constrained Dynamics.

<div align="center">
  <img src="sim.gif"/>
</div>

Inspired by: https://matthias-research.github.io/pages/publications/XPBD.pdf

This system includes various game engine support for ease of usability.

## What it simulates

- **Soft balls**: closed particle shells held by stretch and bending springs plus a volume constraint, so they squash and bounce but keep their volume.
- **Cloth**: square sheets with stretch, shear and bending constraints, self-collision and double-sided rendering. Sheets can hang free, be pinned at the corners, along one edge (a banner) or around the whole border (a trampoline).
- **Contacts**: one XPBD contact model for particle-particle and particle-ground contact, with static and dynamic friction, restitution and rolling resistance.

The solver follows Macklin et al., "Small Steps in Physics Simulation" (2019) and "Detailed Rigid Body Simulation with Extended Position Based Dynamics" (2020): each frame is split into substeps, contacts are found once per frame with a spatial hash and solved every substep, and a velocity pass applies friction and restitution. Damping removes only deformation, so falling, rolling and spinning are preserved.

## Editor and play mode

The app works like a game-engine scene editor. It starts with an empty scene in **Edit** mode, where nothing simulates:

- **Hierarchy** (left): add a ball or a cloth, load the demo scene, clear the scene, and pick objects from the list.
- **Inspector** (right): edit the selected object's position, colour, mass, size and solver settings. Each change rebuilds that object.
- **Simulation** (top): Play, Pause, Step and Stop, plus timings and the solver settings.

**Play** starts the simulation from the authored scene. Objects can still be added, grabbed and deleted while playing, but those are runtime changes: **Stop** returns to Edit mode and rebuilds the scene exactly as it was before Play. The inspector is read-only while playing.

| Input | Edit mode | Play mode |
| --- | --- | --- |
| Left click | Select the object under the cursor (empty space deselects) | Same |
| Right-drag | Move the object under the cursor | Grab and drag it physically |
| Delete | Delete the selection | Delete it until Stop |
| Left-drag | Orbit the camera | Same |
| Mouse wheel | Zoom | Same |

## Building and testing

Open `src/pdb-sim.sln` in Visual Studio 2022 or later, or build from a developer prompt:

```
msbuild src/pdb-sim.vcxproj -p:Configuration=Release -p:Platform=x64
```

Run from `src/` so the shaders are found. Command-line options:

- `pdb-sim.exe --test` runs the physics regression tests headless and exits with the number of failures.
- `pdb-sim.exe --smoke` runs the editor with scripted input (add objects, click to select, play, grab, delete and add during play, pause, step, stop, move and edit in edit mode, load the demo) and checks after each step, including that Stop restores every particle exactly. It exits with the number of failed checks.
- `pdb-sim.exe --demo` opens the editor with the demo scene loaded.
