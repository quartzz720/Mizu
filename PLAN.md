# Mizu — from a program with windows to a system you can live in

## Where this actually is

Mizu is one program. It draws a desktop, a menu bar and a taskbar; it has a
control panel, a clock, a notepad and a WAV player, and all of them are parts
of the same image compiled together. It arrives as a package and Koi-DOS is
complete without it.

That is not a stopgap: it is what Windows 1.0 through 3.0 in real mode actually
were, and their bundled applications were parts of one image for the same
reason. But it is also the ceiling. Everything below is about raising it.

**What it cannot do today, in one sentence:** start a program and stay on the
screen. `SYS_RUN` loads a Koi-DOS program into another slot and returns when it
exits — the caller is stopped inside the call, and the program takes the whole
display. Launching anything means the desktop disappears until it is over. That
is DOS's EXEC, and it is not a desktop.

---

## The decision everything else hangs on

**What is a Mizu application?** There are two answers and they lead to
different systems.

**(A) An application is a module Mizu loads into its own address space**, which
is handed a table of function pointers - the Mizu API - and gives back a window
procedure. Mizu calls it to paint, to handle a click, to handle a key.
Cooperative: an application that never returns from its handler stops the
desktop.

This is exactly what Windows 3.0 was, and it needs almost nothing from the
kernel: one call that loads and relocates a program image and returns its entry
point *without entering it*. Our programs are already position-independent with
`R_X86_64_RELATIVE` relocations, and the kernel already has the loader; today
it always enters what it loads.

**(B) An application is an ordinary Koi-DOS program**, in its own address space,
preempted by a scheduler, drawing through system calls.

This is Windows 95, and it needs address spaces, a scheduler, ring 3 and a
window server before a single application can run. Every one of those is real
work and none of them shows anything on screen until they are all finished.

**Take (A) first, and let (B) grow out of it.** The API an application is
written against is the same in both; what changes underneath is who owns the
memory and who takes the processor away. Windows made exactly this journey with
the same applications running throughout, and that is the evidence that the
order works.

---

> **Где мы сейчас (14 августа 2026).** Stage 1 закрыт: кнопка «Пуск», меню из
> установленных пакетов, Run…, выключение с подтверждением. Stage 2 сделан в
> своей главной части — `SYS_LOAD` в ядре, контракт `mizu.h`, NoteEdit живёт
> отдельным `NOTEEDIT.APP`, который Mizu грузит и вызывает. Осталось второе
> приложение (проверка того, что таблица годится не только для одного) и
> Player тоже уехал — `PLAYER.APP`. Два приложения открываются одновременно,
> рабочий стол под ними живой; в MIZU_API ради второго не пришлось добавить
> ничего, и это тот ответ, ради которого второй порт и делался.
>
> Stage 3 сделан: `window_yield()`, очередь событий на время уступки, и `tick`
> у окна — таймер, который что-то делает, а не только перерисовывает. Проверено
> приложением `apps/spin.c` (в пакет не входит): вежливая работа — часы идут,
> невежливая — часы стоят.
>
> Stage 4 начат: `FILES.APP` — браузер файлов в форме Explorer (диски, папки,
> двойной щелчок), `MIZU_API.run` для запуска программ Koi-DOS. Обои ужаты до
> 24 бит (2.2 МБ вместо 2.9). Осталось: просмотрщик картинок как приложение,
> Окно терминала (`TERM.APP`) сделано: `SYS_CAPTURE` в ядре, вывод команды
> собирается в буфер программы, экран остаётся у рабочего стола. Просмотрщик
> картинок (`IMAGE.APP`) и открытие файла приложением (`open_file`/`open_with`)
> тоже. Буфер обмена — в меню NoteEdit. Настройки (`CONTROL.APP`): язык, звук,
> цвета консоли — меняются на месте и сразу записываются. Смена языка
> перекрашивает уже открытое: меню стола, панель, заголовки окон и меню
> загруженных приложений (`MIZU_APP.relabel`). Stage 4 закрыт — дальше Stage 5,
> ring 3.

## Stage 1 — The Start button

Small, entirely visible, and it makes the desktop the thing you launch from
rather than a place four built-in toys live.

- **A Start menu**, in the 9x shape: Programs, Run…, Settings, Shut Down.
- **Programs is not a hardcoded list.** Every installed package wrote
  `\BOOT\DOSGET\<NAME>.PKG` when dosget installed it, naming its directory and
  its files. Mizu reads them, so a package installed yesterday is in the menu
  today without Mizu knowing anything about it. That record already exists for
  the installer's sake; this is its second reader.
- **Launching a Koi-DOS program** stays what it is - `SYS_RUN`, the program
  takes the screen, the desktop comes back when it exits. Honest, and it is how
  Windows 3.0 ran a DOS program too.
- **Shut Down** through the kernel's ACPI path, and a confirmation, because a
  Start menu whose bottom entry turns the machine off without asking is a
  Start menu people learn to fear.

At the end of this stage Mizu is a launcher. Not yet a system, but the thing
somebody would actually use.

---

## Stage 2 — Applications, and the interface they are written against

The stage that turns Mizu from a program into a platform.

**In the kernel: one call.** `SYS_LOAD` - load a program image, apply its
relocations at the address it was loaded to, return the entry point, do not
enter it. Everything it needs exists: `load_segments()` and `relocate()` are
already there and are already used by `SYS_RUN`; what is new is stopping short
of the jump.

**In Mizu: the application contract.** An application's entry point is handed a
pointer to the API table and returns a description of itself:

    MIZU_APP* mizu_main(const MIZU_API* api);

No dynamic linking, no symbol resolution, no relocation of anything but the
program's own pointers - a table of function pointers is the whole interface.
It is what a VxD's service table was, and what every C plugin system settles on
in the end.

**The API is `window.c`, promoted.** It is already the right shape: a window
owns its contents and the library owns the frame, the ordering and the pointer.
Today it is linked into each program; here it becomes something an application
is given rather than something it carries.

**The proof that the interface is real is a port.** NoteEdit comes out of
`mizu.c` and becomes an application in its own file, built separately, loaded
at run time. If it needs anything the API does not have, the API is wrong and
this is when we find out - not later, with five applications written.

At the end of this stage two applications can be open at once and the desktop
is alive underneath them. That is Windows 3.0.

---

## Stage 3 — Cooperative multitasking

Once applications are modules with handlers, this is smaller than it sounds:
the desktop already has an event loop, and a handler that returns is a task
that has yielded.

- **A message queue per window**, and a loop that delivers to whichever window
  has something waiting.
- **Timers**, so a clock ticks and a progress bar moves while something else
  has focus. `repaint_ms` already exists and is the seed of this.
- **`mizu_yield()`** for an application doing something long, so it can stay
  responsive without being interrupted.

Cooperative and not preemptive, deliberately: every application here is one we
compiled, the desktop cannot be hurt by one that behaves, and preemption
without memory protection is a way to corrupt memory faster. Windows 3.0 made
the same trade for the same reason.

**What this costs honestly:** one application that loops forever freezes
everything. That is the trade until Stage 5.

---

## Stage 4 — The applications you need to survive

None of this is architecture; it is the reason the architecture exists.

- **A file browser in a window, and it is not Koi-Commander.** Nobody shipped
  Norton Commander in place of `explorer.exe`, and the reason is that they are
  different things: a two-panel manager is a tool for moving files between two
  places, and the shell's browser is how you *find* things - one place at a
  time, a tree beside it, icons, and double-click to open. Koi-Commander stays
  what it is and stays excellent at it.

  What is reusable is underneath the panels: reading a directory, copying,
  the viewer and the editor. The panels themselves are not.
- **A text editor.** `editcore.c` is already shared between `edit` and the
  notepad.
- **An image viewer.** BMP today - the wallpaper loader already parses it -
  then PNG, which needs an inflate and is a weekend.
- **Settings that change things**: colours, wallpaper, sound, language, and
  the layout gesture, all of which the kernel already exposes.
- **The clipboard between applications**, which the kernel already has and
  which nothing but the commander and the notepad use.
- **A terminal window** running a Koi-DOS command with its output in a window,
  once output redirection can be pointed at something other than a file. That
  one is a kernel change and it is the interesting one.

**And the wallpaper stops being three megabytes.** It is a 24-bit BMP at full
resolution, which is most of the MIZU package and most of what a slow
connection spends its time on. Either a smaller image scaled up, or a format
with a compressor behind it.

---

## Stage 5 — Ring 3, and it belongs here rather than to DOS

This is where Mizu stops being a program and becomes a system.

Koi-DOS is a DOS: one program at a time, ring 0, talking to the hardware, and
that is the contract - it is why a program there can ask the processor its own
name. In Windows 3.x the protected-mode kernel was not part of DOS; it arrived
with the graphical system, took the machine, and ran DOS underneath it. If Mizu
goes that way, isolation belongs to Mizu, and a program run from `Z:\>` keeps
the flat machine DOS programs are written for.

One system, two contracts, each honest about which it is offering.

- **An address space per application**, which the kernel already has the
  machinery for - it builds its own page tables.
- **Applications in ring 3**, with the API table becoming a system call rather
  than a call through a pointer. The interface an application was written
  against does not change; what changes is how the call arrives.
- **A fault kills the application, not the machine.** This is the whole prize:
  "a program crashed" stops meaning "the machine crashed".

There is no V86 mode in long mode, and it does not matter - Windows needed it
to virtualise unmodified real-mode binaries, and our compatibility target is a
system we control. Mizu owns the system call and answers all of it.

---

## Stage 6 — The 9x shape

- **Preemption**, once there is protection to make it safe.
- **Mizu starts from `AUTOEXEC.BAT` and does not return**, with the shell
  underneath as the compatibility layer rather than the host. That is precisely
  what MS-DOS 7 became.
- **Loadable drivers** - the VxD idea. Not needed for any of the above, and
  worth wanting on its own: a machine that can be given a driver without
  rebuilding the kernel is a different kind of machine.

---

## The order, and why

Stages 1 and 2 are the ones that change what Mizu *is*, and 2 is the one that
decides whether everything after it is possible. 3 is small once 2 exists. 4 is
the largest in hours and the smallest in risk - it is applications, and every
one of them makes the system more usable the day it lands. 5 is the largest in
difficulty. 6 is a consequence.

Nothing here needs Koi-DOS to change except one kernel call in Stage 2, and one
more in Stage 4 for a terminal window. That is the measure of whether the split
into two projects was right: if this plan needed the kernel rewritten at every
step, Mizu would still belong in that tree.

**Between 3.x and 9x** is the honest description of the destination. The
windowing, the cooperative loop and the applications-in-one-space are 3.x. The
Start button, the taskbar, the installed-program list and eventually the
protection are 9x. Nobody has to choose.
