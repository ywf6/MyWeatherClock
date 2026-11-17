"use strict";

const byId = (id) => document.getElementById(id);
const phaseContent = {
  offline: ["设备离线", "外置 C3 连上小智服务后，这里会显示问答进度。"],
  idle: ["等待提问", "按板上的 K2 开始录音，按 K3 返回时钟。"],
  recording: ["正在录音", "再按一次 K2 可结束录音。"],
  processing: ["正在生成", "服务已收到录音，正在生成回答。"],
  playing: ["正在播报", "回答正在传给外置 C3 播放。"],
  displaying: ["正在上屏", "正在向 STM32 传送回答文字。"],
  done: ["问答完成", "按 K2 可开始下一轮问答。"],
  too_short: ["录音太短", "按 K2 再说一次。"],
  error: ["问答失败", "请查看服务终端日志，再按 K2 重试。"],
  unavailable: ["服务未连接", "确认小智服务已启动，再刷新状态。"],
};
const steps = [...document.querySelectorAll("[data-step]")];
const clock = new Intl.DateTimeFormat("zh-CN", {hour: "2-digit", minute: "2-digit", second: "2-digit", hour12: false});
let entries = [];
let selected = null;
let serviceStartedAt = null;
let refreshing = false;
let copyTimeout;

function label(id, value) {
  const el = byId(id);
  if (el.textContent !== value) el.textContent = value;
}

function phase(value) {
  const [title, detail] = phaseContent[value] || phaseContent.idle;
  label("phase-label", title);
  label("phase-detail", detail);
  byId("live-panel").dataset.phase = value;
  const active = steps.findIndex((step) => step.dataset.step === value);
  steps.forEach((step, index) => {
    step.classList.toggle("past", value === "done" || (active >= 0 && index < active));
    if (index === active) step.setAttribute("aria-current", "step");
    else step.removeAttribute("aria-current");
  });
}

function showAnswer() {
  const entry = entries.find((item) => item.id === selected) || entries[0];
  label("answer-title", entry && entry.id !== entries[0].id ? "历史回答" : "最近回答");
  label("answer", entry ? entry.answer : "有一轮成功的问答后，回答会显示在这里。");
  byId("answer").classList.toggle("is-empty", !entry);
  byId("answer-meta").hidden = !entry;
  byId("copy").disabled = !entry;
  if (entry) {
    label("input-time", entry.input_seconds.toFixed(2) + " 秒");
    label("elapsed-time", entry.elapsed_seconds.toFixed(2) + " 秒");
    label("screen-status", {confirmed: "已确认", unconfirmed: "未确认", skipped: "未请求"}[entry.screen] || "—");
  }
  byId("history").querySelectorAll("button").forEach((button) => {
    button.setAttribute("aria-pressed", String(Boolean(entry) && Number(button.dataset.entry) === entry.id));
  });
}

function showHistory(history) {
  const changed = history.length !== entries.length || history.some((entry, i) => entry.id !== entries[i].id || entry.at !== entries[i].at);
  entries = history;
  byId("history-empty").hidden = entries.length > 0;
  if (changed) {
    const list = entries.map((entry) => {
      const li = document.createElement("li");
      const button = document.createElement("button");
      button.type = "button";
      button.dataset.entry = entry.id;
      const title = document.createElement("span");
      title.textContent = entry.answer;
      const timestamp = document.createElement("time");
      timestamp.textContent = clock.format(entry.at * 1000);
      timestamp.dateTime = new Date(entry.at * 1000).toISOString();
      button.append(title, timestamp);
      li.append(button);
      return li;
    });
    byId("history").replaceChildren(...list);
    if (!entries.some((entry) => entry.id === selected)) selected = null;
  }
  showAnswer();
}

function render(data) {
  if (serviceStartedAt !== data.started_at) selected = null;
  serviceStartedAt = data.started_at;
  byId("gateway-status").dataset.tone = "positive";
  label("gateway-label", "服务在线");
  label("updated-at", "更新于 " + clock.format(Date.now()));
  const active = data.devices.find((device) => steps.some((step) => step.dataset.step === device.phase));
  const device = active || data.devices[0];
  byId("device-status").dataset.tone = device ? "positive" : "neutral";
  label("device-label", device ? data.devices.length + " 台在线" : "未连接");
  label("device-id", device ? device.label : "—");
  label("backend", data.backend === "dashscope" ? "百炼" + (data.model ? " · " + data.model : "") : "本地回声");
  label("tools", device ? (device.tools_ready ? "已就绪" : "待就绪") : "—");
  label("completed", String(data.completed));
  phase(device ? device.phase : "offline");
  showHistory(data.history);
}

async function refresh() {
  if (refreshing) return;
  refreshing = true;
  try {
    const response = await fetch("/api/status", {cache: "no-store", signal: AbortSignal.timeout(4000)});
    if (!response.ok) throw new Error("状态请求失败");
    render(await response.json());
  } catch {
    byId("gateway-status").dataset.tone = "negative";
    byId("device-status").dataset.tone = "neutral";
    label("gateway-label", "服务未连接");
    label("device-label", "状态未知");
    label("device-id", "—");
    label("tools", "—");
    label("updated-at", "正在尝试重连");
    phase("unavailable");
  } finally {
    refreshing = false;
  }
}

byId("history").addEventListener("click", (event) => {
  const button = event.target.closest("[data-entry]");
  if (!button) return;
  const id = Number(button.dataset.entry);
  selected = id === entries[0]?.id ? null : id;
  showAnswer();
});

byId("copy").addEventListener("click", async () => {
  const entry = entries.find((item) => item.id === selected) || entries[0];
  if (!entry) return;
  let message;
  try {
    await navigator.clipboard.writeText(entry.answer);
    message = "已复制";
  } catch {
    message = "复制失败";
  }
  label("copy", message);
  label("copy-result", message);
  clearTimeout(copyTimeout);
  copyTimeout = setTimeout(() => label("copy", "复制文字"), 1600);
});

byId("refresh").addEventListener("click", refresh);
refresh();
setInterval(refresh, 2000);
