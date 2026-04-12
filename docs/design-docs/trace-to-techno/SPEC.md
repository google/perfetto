# Trace-To-Techno

## Vision

Trace-To-Techno is an experimental project to convert a trace file into a waveform (a techno music track).
The overall idea is to be able to map a trace, which is a complex set of tracks, slices, counters and flows into a music file, in a way that trace input contributes to the "entropy" and spreads, in a reproducible fashion, in the audio spectrum.
The reason why we want to do this is to later use the generate wav to apply the existing ML research to find similar traces (or better portions of a trace) from a corpus of millions of traces. This is predicated on our ability to create enough "musical entropy" and have the right intuions about how to map trace elements to synths. 

The intuition is that, in a way, a trace is like a techno sound track: An Android trace often consists in hundreds of threads that are working together to put pixels on screen with a semi-regular cadence of 60/100/120 FPS (Frames per second), fps is very device-dependent.
A techno track has  a cadence of 120-150 BPM. so with some time dilation they should mapw ell into each other

## Design docs

- [Background on Modular Synths](background-on-synths.md) -- how modular
  synthesizers work and how the concepts map to this project.
- [TraceProcessor Implementation](trace-processor-design.md) -- current
  architecture of the synth engine in TP: code layout, RPC interface, module
  system, render pipeline, and CLI usage.

## Overall architecture

TraceProcessor (TP) should become a modular synthesizer. I am thinking of synths like Monark or "Massive X" or the open-source "Surge XT".
The beauty of them is that they are all pure mathematical functions.

TP should have the building blocks that allow turning the input trace (or a portion of it, i.e. selected tracks and time range) into a wav.
The reason why I want in TP is because it I envision a dual use-case:
1. Development/experimentation phase: the UI will configure the blocks in TP, determine the wiring -> let TP do the synth -> get the waveform back and play in the UI via webaudio. This enables research
2. Running in batch: once we have figured out the config, I want to be able to batch-convert some traces into .wav using purely TP cmdline, without the UI.

The overall view is that all the blocks required for the synth should live in TP. However, HOW the blocks are connected to each other (Think about the synth wiring) and HOW they are connected to the trace inputs should be defined in a file (proto or JSON TBD) which can be dynamically pushed to TP to rewire things.

This is the overall idea, which will require some later refinement
- TP has the synth code in the form of C++ blocks. THey take as input the config proto, which defines the wiring, and the trace database itself and emits a bytes blob with the .wav.
- The UI will have a frontend, similar to the Data Explore page, to setup the synth wiring and generate the proto. The proto will be pushed into TP when the wiring is changed.
- TP has a custom RPC endpoint in trace_processor.proto, which can be used to pass the synth config, and to require the synthesis of a given set of tracks for a given time range. This RPC returns the synth waveform.
- The UI will then play this waveform using webaudio

## Milestones

Each milestone should be a dedicated agent session.

### 1 Brainstorm to decide a plan

We need to decide how the overall architecture looks like.
For sure TP will have some C++ code for the synth blocks, which can be dynamically connected.
The thing to decide is how the various controls (think of the various knobs you have on a synth) work.
For sure we want everythign to be deterministic, nobody is going to "play live" or adjust knobs while we play.
All The VFO inputs and all the control knobs must be statically mapped to properties of the trace (e.g. specific tracks and slices).
TP must be able to generate the synth everything without any extra input or human interventon, using as input only: The trace contents itself (i.e. the various tables) and proto that defines the wiring.


### 2 Basic architecture

This is to setup the codebase come up with the right interaction between TP and the UI, before we delve into specific blocks.
As a result from this milestone we want few very basic blocks and a simple config language to map them to tracks (e.v. via basic regexes) to nail the UX of and the interaction of UI and TP.


### 2 Coming up with synth blocks

In this milestone we want to write the C++ code for the blocks.
We should do some research about which synth modules are more appropriate for techno.

### 3 Discuss the mapping of trace to synth

We should research and discuss here how to map portions of the trace to functions that drive the synth.
E.g. things like "the kernel threads will operate in the base octaves / bass line", we need to figure out the tempo generation and so on.
