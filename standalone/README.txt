RidingPlugin standalone launcher (no RE_Kenshi required)
=========================================================

This folder contains RidingPlugin.dll, RidingBootstrap.dll, KenshiLib.dll
0.4.0, and the matching RE_Kenshi/RVAs tables.  Run RidingLauncher.exe; pass
the Kenshi install directory as its first argument when auto-detection fails.

Steam 1.0.65 is loaded directly.  Steam 1.0.68 is handled using the same
official compatibility method as RE_Kenshi: the bundled Courgette patch makes
a private kenshi_x64_riding_1.0.65.exe beside the installed game executable.
The original Kenshi_x64.exe is never replaced.  Keep tools/courgette64.exe and
tools/kenshi_x64.exe.patch beside the launcher.

Do not run this together with RE_Kenshi.dll.  The launcher warns when that
file is present and the bootstrap refuses to double-load it.
