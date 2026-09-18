# Changing the BSoD Emoticon

## Target / Expected Result

The goal of this walkthrough is to use **WinDbg** to locate the string used by the Windows kernel when displaying the BSOD and, in a controlled debugging environment, change the emoticon from:

```text
:(
```

to:

```text
:)
```

By the end of the walkthrough, you should understand how to:

1. Locate a kernel function using WinDbg symbols.
2. Disassemble that function.
3. Follow a call to the function responsible for displaying critical strings.
4. Identify the memory containing the `:(` Unicode string.
5. Inspect the raw bytes of that string.
6. Modify the `(` character in memory to `)`.

### Important expectation

This is a **runtime memory modification** performed through WinDbg. It does not modify the Windows kernel file on disk.

The change is therefore intended as a debugging/research demonstration. It may disappear after a reboot, and kernel builds can change the location or implementation of the relevant code and string.

---

# 1. Locate the BSOD Display Function

Start by looking for the function responsible for displaying the bug-check screen: [Source](https://pagedout.institute/download/PagedOut_003.pdf)

```text
x nt!BgpFwDisplayBugCheckScreen
```

This searches the `nt` module for the symbol `BgpFwDisplayBugCheckScreen`.

- `x` - WinDbg's **examine symbols** command.
- `nt!` - specifies the Windows kernel module `nt`, commonly associated with `ntoskrnl.exe`.
- `BgpFwDisplayBugCheckScreen` - the function/symbol being searched for.

For example, WinDbg may return:

```text
fffff806`6334ab70 nt!BgpFwDisplayBugCheckScreen
```

The address shown is the virtual address of the function in the currently loaded kernel image.

---

# 2. Disassemble the Function

Next:

```text
uf nt!BgpFwDisplayBugCheckScreen
```

This tells WinDbg to display the assembly instructions belonging to the function.

- `u` - unassemble.
- `f` - function.
- `uf` - unassemble a function.

While examining the function, we're interested in code related to displaying text on the bug-check screen.

Eventually, we find a call such as:

```text
nt!BgpFwDisplayBugCheckScreen+0x131:

fffff806`6334aca1
e8fef0ffff    call    nt!BcpDisplayCriticalString
                  (fffff806`63349da4)
```

The function:

```text
nt!BcpDisplayCriticalString
```

is particularly interesting because its name indicates that it handles displaying a critical string.

---

# 3. Examine the Code Before the Call

We can inspect the instructions leading up to the call.

For example:

```text
nt!BgpFwDisplayBugCheckScreen+0x116:

fffff806`6334ac86
418b54f40c    mov     edx,dword ptr [r12+rsi*8+0Ch]

fffff806`6334ac8b
448bcb        mov     r9d,ebx

fffff806`6334ac8e
488d0d2b990200
              lea     rcx,[nt!ExpLeapSecondRegkeyPath+0x28e0
              (fffff806`633745c0)]

fffff806`6334ac95
443bff        cmp     r15d,edi

fffff806`6334ac98
7507          jne     nt!BgpFwDisplayBugCheckScreen+0x131
```

The important instruction here is:

```text
lea rcx,[nt!ExpLeapSecondRegkeyPath+0x28e0
         (fffff806`633745c0)]
```

This places the address of a data structure/string descriptor into `RCX`.

The interesting address is therefore:

```text
fffff806`633745c0
```

---

# 4. Read the Unicode String

We can inspect the pointer stored inside that structure:

```text
du poi(fffff806`633745c0+0x8)
```

Breaking this down:

- `du` - display a Unicode string.
- `poi(...)` - dereference the address and use the resulting value as an address.
- `fffff806`633745c0` - address of the structure.
- `+0x8` - access the pointer stored eight bytes into the structure.

WinDbg returns:

```text
fffff806`6338a6f8  ":("
```

We've found the string.

The string is stored elsewhere in memory, at:

```text
fffff806`6338a6f8
```

---

# 5. Verify the Raw Bytes

Before modifying anything, verify the contents of the memory.

Run:

```text
db fffff806`6338a6f8 L4
```

Where:

- `db` - display bytes.
- `fffff806`6338a6f8` - starting address.
- `L4` - display four bytes.

The output should resemble:

```text
fffff806`6338a6f8  3a 00 28 00  :.(.
```

The bytes correspond to:

| Byte | Meaning |
|---|---|
| `3A` | `:` |
| `00` | Unicode null byte |
| `28` | `(` |
| `00` | Unicode null byte |

Because the string is Unicode, each ASCII character occupies two bytes:

```text
:  → 3A 00
(  → 28 00
```

Therefore:

```text
3A 00 28 00
```

represents:

```text
:(
```

---

# 6. Change `(` to `)`

The ASCII/Unicode value for `)` is:

```text
29
```

The original bytes are:

```text
3A 00 28 00
```

We only need to change:

```text
28
```

to:

```text
29
```

The WinDbg command is:

```text
eb fffff806`6338a6f8+2 29
```

Breaking it down:

- `eb` - enter byte.
- ``fffff806`6334ab70`` - address of the beginning of the string.
- `+2` - move two bytes forward to the `(` character.
- `29` - hexadecimal value for `)`.

The memory changes from:

```text
3A 00 28 00
```

to:

```text
3A 00 29 00
```

Which corresponds to:

```text
:(
```

becoming:

```text
:)
```

---

# 7. Verify the Change

Run:

```text
db fffff806`6338a6f8 L4
```

The bytes should now be:

```text
3A 00 29 00
```

You can also display the Unicode string again:

```text
du fffff806`6338a6f8
```

The result should show:

```text
:)
```

When the bug-check screen uses this string, the displayed emoticon will therefore reflect the modified value.

---

# What We Actually Did

The important part of the process is not the specific address.

The addresses are specific to the particular Windows kernel image being debugged and can change between builds.

The useful workflow is:

```text
Find symbol
    ↓
Disassemble function
    ↓
Find string-display call
    ↓
Inspect arguments/data used by the call
    ↓
Locate Unicode string
    ↓
Inspect raw bytes
    ↓
Modify the required byte
```

In this example:

```text
BgpFwDisplayBugCheckScreen
            ↓
BcpDisplayCriticalString
            ↓
Unicode string
            ↓
3A 00 28 00
            ↓
3A 00 29 00
            ↓
:(
            ↓
:)
```

---

# Notes

## Addresses are not universal

The addresses shown in this walkthrough belong to one particular loaded kernel image.

For example:

```text
fffff806`6338a6f8
```

should **not** be expected to contain the same string on another Windows installation or build.

Use the symbols and disassembly of the kernel currently being debugged to locate the relevant data.

## Runtime modification

`eb` changes the contents of memory in the running debugging target.

It does not patch:

```text
C:\Windows\System32\ntoskrnl.exe
```

on disk.

A reboot or other change to the running kernel image can therefore remove the modification.

## Windows updates

Kernel internals are implementation details and can change between Windows versions and builds. Function layouts, symbols, instructions, and string locations may all change.

Consequently, the exact offsets and addresses in this document should be treated as examples rather than permanent values.

---

# WinDbg Commands Used

| Command | Purpose |
|---|---|
| `x` | Search/examine symbols |
| `uf` | Disassemble a function |
| `du` | Display Unicode string |
| `poi` | Dereference a pointer |
| `db` | Display memory as bytes |
| `eb` | Enter/write a byte |

The core commands used in this walkthrough are:

```text
x nt!BgpFwDisplayBugCheckScreen

uf nt!BgpFwDisplayBugCheckScreen

du poi(<address>+0x8)

db <string_address> L4

eb <string_address>+2 29

du <string_address>
```

Replace the example addresses with the addresses discovered in the kernel being debugged.
