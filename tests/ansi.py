#   MCprog - A programmer for the Motorola MC micro radio family,
#            replacing the 1987 Radio Service Software
#
#   Copyright (C) 2026  Felix Erckenbrecht, DG1YFE
#
#    This file is part of MCprog.
#
#    MCprog is free software: you can redistribute it and/or modify
#    it under the terms of the GNU General Public License as published by
#    the Free Software Foundation, either version 3 of the License, or
#    (at your option) any later version.
#
#    MCprog is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU General Public License for more details.
#
#    You should have received a copy of the GNU General Public License
#    along with MCprog.  If not, see <http://www.gnu.org/licenses/>.
#
#    SPDX-License-Identifier: GPL-3.0-or-later
"""Tiny ANSI.SYS-ish 80x25 screen so we can read what the program drew."""
class Screen:
    def __init__(self, w=80, h=25):
        self.w, self.h = w, h
        self.buf = [[' '] * w for _ in range(h)]
        self.r = self.c = 0
        self.saved = (0, 0)
        self.scroll = []          # rows that scrolled off the top, oldest first
    def feed(self, data: bytes):
        i = 0
        while i < len(data):
            ch = data[i]
            if ch == 0x1B and i + 1 < len(data) and data[i+1] == ord('['):
                j = i + 2
                while j < len(data) and not (0x40 <= data[j] <= 0x7E): j += 1
                if j >= len(data): break
                params = data[i+2:j].decode('latin1'); cmd = chr(data[j])
                nums = [int(x) for x in params.replace(';;',';0;').split(';') if x.isdigit()]
                if cmd == 'H' or cmd == 'f':
                    self.r = (nums[0] - 1) if len(nums) > 0 else 0
                    self.c = (nums[1] - 1) if len(nums) > 1 else 0
                elif cmd == 'J' and (not nums or nums[0] == 2):
                    self.buf = [[' '] * self.w for _ in range(self.h)]; self.r = self.c = 0
                elif cmd == 'K':
                    for x in range(self.c, self.w): self.buf[self.r][x] = ' '
                elif cmd == 'A': self.r = max(0, self.r - (nums[0] if nums else 1))
                elif cmd == 'B': self.r = min(self.h-1, self.r + (nums[0] if nums else 1))
                elif cmd == 'C': self.c = min(self.w-1, self.c + (nums[0] if nums else 1))
                elif cmd == 'D': self.c = max(0, self.c - (nums[0] if nums else 1))
                elif cmd == 's': self.saved = (self.r, self.c)
                elif cmd == 'u': self.r, self.c = self.saved
                i = j + 1; continue
            if ch == 0x0D: self.c = 0
            elif ch == 0x0A:
                self.r += 1
                if self.r >= self.h:
                    self.r = self.h - 1
                    self.scroll.append(self.buf.pop(0)); self.buf.append([' '] * self.w)
            elif ch == 0x08: self.c = max(0, self.c - 1)
            elif ch == 0x07: pass
            elif 32 <= ch < 127 or ch >= 160:
                if self.r < self.h and self.c < self.w: self.buf[self.r][self.c] = chr(ch)
                self.c += 1
                if self.c >= self.w: self.c = 0; self.r = min(self.h - 1, self.r + 1)
            i += 1
        return self
    def text(self):
        return '\n'.join(''.join(row).rstrip() for row in self.buf)
    def full(self):
        """Scrollback + screen — for listings that are longer than 25 lines."""
        return '\n'.join(''.join(row).rstrip() for row in self.scroll + self.buf)
