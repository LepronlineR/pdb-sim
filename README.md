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

## Controls

| Input | Action |
| --- | --- |
| Left-drag | Orbit the camera |
| Right-drag | Grab and drag the object under the cursor |
| Mouse wheel | Zoom |

The panel shows frame and physics timings and lets you pause or single-step, reset the scene, change substeps, iterations, gravity, friction and restitution, and spawn balls and cloths.

## Building and testing

Open `src/pdb-sim.sln` in Visual Studio 2022 or later, or build from a developer prompt:

```
msbuild src/pdb-sim.vcxproj -p:Configuration=Release -p:Platform=x64
```

Run from `src/` so the shaders are found. Two command-line modes help check changes:

- `pdb-sim.exe --test` runs the physics regression tests headless and exits with the number of failures.
- `pdb-sim.exe --smoke` runs the app with scripted input (grab and drag, release, reset the scene, spawn a ball and a cloth) and exits with 0 if every step worked.
