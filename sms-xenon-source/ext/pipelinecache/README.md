# Pipeline cache saves

With the Vulkan renderer (`renderer = vulkan`), the pipelines the driver builds
are kept in a cache file (`vk-pipelines-*.bin`, in `%LOCALAPPDATA%\sms-port` on Windows), so
a later run does not build them again. Saving it means asking the driver for
the whole cache, which takes a few milliseconds (about 3 ms for 5 MB on an
NVIDIA RTX 5090), and in play that is a visible hitch at 120 fps.

So the cache is saved only when that goes unseen, and only when pipelines were
built since the last save:

- when the pause menu, the map or a save screen opens;
- when a stage starts loading;
- when the port's settings menu opens;
- when the game quits.

A crash loses only the pipelines built since the last of these; they are built
again when next needed.
