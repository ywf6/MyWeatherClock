# 历史文字问答控制器；当前 Qt 后台仍复用其中的 render_chat()。
# 理解 Qt 主链路时先读 render_chat，Tk ChatController 可在第二轮阅读时再看。
"""Text in place of ASR; real model calls remain on the PC."""
from pathlib import Path
import os
import queue
import sys
import threading
import time
import tkinter as tk
from tkinter import ttk

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT.parent / 'ai-assistant'))
from relay import provider
from weather_display import XianWeather


def load_provider():
    # pythonw started by Explorer may predate changes to user environment variables.
    if os.name == 'nt':
        import winreg
        for name in ('DASHSCOPE_API_KEY', 'DASHSCOPE_BASE_URL', 'WEATHERCLOCK_MODEL'):
            if os.environ.get(name):
                continue
            for hive, subkey in ((winreg.HKEY_CURRENT_USER, 'Environment'),
                                 (winreg.HKEY_LOCAL_MACHINE, r'SYSTEM\CurrentControlSet\Control\Session Manager\Environment')):
                try:
                    with winreg.OpenKey(hive, subkey) as key:
                        value, _ = winreg.QueryValueEx(key, name)
                    if value:
                        os.environ[name] = value
                        break
                except OSError:
                    pass
    return provider()


# 使用 Windows 微软雅黑生成 216x208、1bpp 中文点阵；问题最多 2 行，回答最多 6 行，超出加省略号。
# 返回原始像素而非 PNG，大小固定 5616 字节；STM32 驱动按每行 27 字节解码。
def render_chat(question, answer):
    from PIL import Image, ImageDraw, ImageFont
    image = Image.new('1', (216, 208))
    draw = ImageDraw.Draw(image)
    font = ImageFont.truetype('C:/Windows/Fonts/msyh.ttc', 16)

    def wrap(text):
        lines, line = [], ''
        for char in text:
            if char == '\n' or draw.textlength(line + char, font=font) > 212:
                lines.append(line)
                line = ''
            if char != '\n':
                line += char
        if line:
            lines.append(line)
        return lines

    def limit(lines, count):
        if len(lines) > count:
            lines = lines[:count]
            while lines[-1] and draw.textlength(lines[-1] + '…', font=font) > 212:
                lines[-1] = lines[-1][:-1]
            lines[-1] += '…'
        return lines

    lines = limit(wrap('问：' + question), 2) + [''] + limit(wrap('答：' + answer), 6)
    for index, line in enumerate(lines):
        draw.text((1, index * 22), line, font=font, fill=1)
    assert len(image.tobytes()) == 5616
    return image


class ChatController:
    def __init__(self, app, parent):
        self.app = app
        self.events = queue.Queue()
        self.generation = 0
        self.active = False
        self.stage = ''
        self.rev = None
        self.deadline = 0
        self.worker = None
        self.bitmap = None
        self.preview = None
        self.image_ref = None
        self.weather = XianWeather()
        self.pc_only = tk.BooleanVar(value=False)
        self.request_pc_only = False
        self.question = tk.StringVar(value='西安现在天气怎么样，出门怎么穿？')
        self.status = tk.StringVar(value='文字模拟语音识别 · 调用阿里云模型 · 不录音、不播报')
        row = ttk.Frame(parent, padding=(8, 8))
        row.pack(fill='x')
        self.entry = ttk.Entry(row, textvariable=self.question, font=('Microsoft YaHei UI', 11))
        self.entry.pack(side='left', fill='x', expand=True, padx=(0, 10))
        self.entry.bind('<Return>', lambda event: self.submit())
        self.button = ttk.Button(row, text='发送 · 模拟说话', command=self.submit, style='Accent.TButton')
        self.button.pack(side='left')
        ttk.Checkbutton(row, text='仅电脑测试', variable=self.pc_only).pack(side='left', padx=(8, 0))
        ttk.Button(row, text='取消', command=lambda: self.cancel('已取消；迟到回答不会写入屏幕')).pack(side='left', padx=(8, 0))
        ttk.Label(parent, textvariable=self.status, foreground='#697b91', padding=(8, 2)).pack(anchor='w')
        self.answer = tk.Text(parent, height=5, font=('Microsoft YaHei UI', 11), wrap='word', relief='flat', padx=10, pady=6)
        scroll = ttk.Scrollbar(parent, command=self.answer.yview)
        self.answer.configure(yscrollcommand=scroll.set, state='disabled')
        scroll.pack(side='right', fill='y')
        self.answer.pack(fill='both', expand=True)

    def show_answer(self, text):
        self.answer.configure(state='normal')
        self.answer.delete('1.0', 'end')
        self.answer.insert('1.0', text)
        self.answer.configure(state='disabled')

    def cancel(self, reason):
        self.generation += 1
        was_active = self.active
        self.active = False
        self.stage = ''
        self.rev = None
        if was_active:
            self.status.set(reason)
        self.button.configure(state='normal')

    def submit(self):
        a = self.app
        question = self.question.get().strip()
        if self.active or (self.worker and self.worker.is_alive()):
            self.status.set('上一请求仍在结束，请稍候再发送')
            return
        if not question or len(question) > 96:
            self.status.set('请输入 1–96 个字符')
            return
        pc_only = self.pc_only.get()
        if a.demo and not pc_only:
            self.status.set('离线演示不调用真实 AI；请连接 COM11')
            return
        if not pc_only and (not a.link or not a.handshake or not a.state or time.monotonic() - a.last_state > 3):
            self.status.set('请先连接开发板并完成握手')
            return
        if not pc_only and 'chat_bitmap' not in getattr(a, 'capabilities', ''):
            self.status.set('当前固件不支持 AI 显示，请烧录本次 ai-v2 固件')
            return
        if a.flow or (not pc_only and a.state['page'] not in ('HOME', 'CHAT')):
            self.status.set('请等待主页或对话页就绪后发送')
            return
        try:
            self.ask = load_provider()
        except ValueError as error:
            self.status.set(str(error))
            return
        self.generation += 1
        self.request_pc_only = pc_only
        self.active = True
        self.stage = 'wake'
        self.rev = None
        self.current_question = question
        self.preview = None
        self.app.last_visual = None
        self.button.configure(state='disabled')
        self.show_answer('你：' + question + '\n\nAI：等待回答…')
        self.status.set('正在唤醒开发板…')
        self.deadline = time.monotonic() + 8
        if pc_only:
            self.start_model({'rev': 0, 'valid': 0})
        elif a.state['page'] == 'HOME':
            a.send('WAKE')
        else:
            self.on_state(a.state)

    def on_state(self, state):
        if not self.active or self.request_pc_only:
            return
        if self.stage == 'wake':
            if state['page'] == 'FACE':
                self.stage = 'face'
            elif state['page'] == 'CHAT':
                self.start_model(state)
        elif self.stage == 'face':
            if state['page'] == 'HOME':
                self.cancel('唤醒已取消')
            elif state['page'] == 'CHAT':
                self.start_model(state)
        elif state['page'] != 'CHAT' or state['rev'] != self.rev:
            self.cancel('页面已离开，迟到回答不会写入开发板')

    def start_model(self, state):
        self.stage = 'model'
        self.rev = state['rev']
        self.deadline = time.monotonic() + 50
        self.started = time.monotonic()
        if not self.request_pc_only:
            self.app.send(f'THINK {self.rev}')
        self.status.set('阿里云模型正在回答…（电脑联网，密钥留在本机）')
        token = self.generation
        indoor = (state['temp10'], state['humidity10']) if state['valid'] & 4 else None
        question, ask = self.current_question, self.ask

        def work():
            try:
                outdoor = self.weather.get()
                answer = ask(question, indoor, outdoor).strip()
                preview = render_chat(question, answer)
                self.events.put((token, answer, preview, None))
            except Exception as error:
                # Do not serialize Request headers, credentials or provider response bodies.
                safe = str(error) if isinstance(error, ValueError) else type(error).__name__
                self.events.put((token, None, None, safe))

        self.worker = threading.Thread(target=work, daemon=True)
        self.worker.start()

    def transmit(self, command, expected, next_offset=None):
        self.expected = expected
        self.next_offset = next_offset
        self.deadline = time.monotonic() + 3
        self.app.send(command)

    def next_chunk(self):
        if self.offset == len(self.bitmap):
            self.transmit(f'LCDEND {self.generation}', 'LCDEND')
        else:
            chunk = self.bitmap[self.offset:self.offset + 96]
            self.transmit(f'LCDDATA {self.generation} {self.offset} {chunk.hex()}', 'LCDDATA', self.offset + len(chunk))

    def on_frame(self, kind, data):
        if not self.active or self.request_pc_only:
            return
        if kind == 'ERR':
            self.cancel('开发板拒绝请求：' + data.get('reason', '未知'))
        elif self.stage == 'upload' and kind == 'ACK' and data.get('cmd') == self.expected:
            if data.get('id') != str(self.generation):
                return
            if self.expected == 'LCDDATA':
                if data.get('next') != str(self.next_offset):
                    self.cancel('屏幕数据偏移校验失败')
                    return
                self.offset = self.next_offset
            if self.expected == 'LCDEND':
                self.active = False
                self.stage = ''
                self.status.set(f'回答完成 · {self.model_seconds:.1f} 秒 · 开发板已接收中文摘要；下方可查看完整回答')
                self.button.configure(state='normal')
                self.app.record('AI 回答已收到，中文点阵已通过开发板校验并提交显示')
                self.app.last_visual = None
                self.app._draw('CHAT')
            else:
                self.next_chunk()

    def tick(self):
        try:
            token, answer, preview, error = self.events.get_nowait()
        except queue.Empty:
            pass
        else:
            if self.active and token == self.generation:
                if error:
                    self.cancel('AI 请求失败：' + error)
                    self.show_answer('你：' + self.current_question + '\n\n请求失败：' + error)
                else:
                    self.model_seconds = time.monotonic() - self.started
                    self.show_answer('你：' + self.current_question + '\n\nAI：' + answer)
                    self.preview = preview
                    if self.request_pc_only:
                        self.active = False
                        self.stage = ''
                        self.status.set(f'真实 AI 回答完成 · {self.model_seconds:.1f} 秒 · 仅电脑测试，未使用板上数据、未写入屏幕')
                        self.button.configure(state='normal')
                        return
                    self.bitmap = preview.tobytes()
                    self.offset = 0
                    self.stage = 'upload'
                    self.status.set('已收到真实 AI 回答，正在传送中文摘要到开发板…')
                    self.transmit(f'LCDSTART {self.generation} {self.rev} {sum(self.bitmap)}', 'LCDSTART')
        if self.active and time.monotonic() > self.deadline:
            self.cancel('操作超时，可重新发送；已有回答保留在电脑上')

    def draw_preview(self, canvas):
        if self.preview is None:
            return False
        from PIL import ImageTk, ImageOps
        colored = ImageOps.colorize(self.preview.convert('L'), '#0c1620', '#ffffff')
        self.image_ref = ImageTk.PhotoImage(colored)
        canvas.create_image(12, 64, anchor='nw', image=self.image_ref)
        return True
