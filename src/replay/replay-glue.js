// The record/replay commands that are easiest to write in JS, on top of an
// in-process inspector session. Runs in a context of its own in each isolate, and
// isn't instrumented. object.js and preview.js are as they are in Replay's Node
// fork (node/lib/internal/recordreplay), with the requires removed.
(function (rr) {
"use strict";

function assert(v) {
  if (!v) {
    log(`Assertion failed ${Error().stack}`);
    throw new Error("Assertion failed");
  }
}

function log(text) {
  rr.log(text);
}

let gNextMessageId = 1;

function sendMessage(method, params) {
  const id = gNextMessageId++;
  const response = JSON.parse(rr.sendCDPMessage(JSON.stringify({ method, params, id })));
  if (response.error) {
    throw new Error(`${method}: ${response.error.message}`);
  }
  return response.result;
}

// Manage association between remote objects and protocol object IDs.


// Map protocol ObjectId => RemoteObject
const gProtocolIdToObject = new Map();

// Map RemoteObject.objectId => protocol ObjectId
const gObjectIdToProtocolId = new Map();

// Map protocol ScopeId => Debugger.Scope
const gProtocolIdToScope = new Map();

let gNextObjectId = 1;

// The inspector keeps everything it wraps alive until its group is released. Nothing
// a client was told about one pause means anything at the next, so all of it goes when
// the replay moves on: the group for what's read here, and the inspector's own for
// call frames. Without this a replay's heap grows with every read until V8 runs out.
const ObjectGroup = "replay";

// Objects a client is told about that the inspector never wrapped: a CSS rule
// as the inspector's CSS agent describes it has no object of the page's behind
// it. Protocol ObjectId => { className, preview }.
const gSyntheticObjects = new Map();

// The embedder's script for a page's DOM, and what it's given to work with.
// Made when a command or a preview first needs it, for one pause.
let gDomBundle;

function clearPauseDataCallback() {
  gProtocolIdToObject.clear();
  gObjectIdToProtocolId.clear();
  gProtocolIdToScope.clear();
  gSyntheticObjects.clear();
  gDomBundle = undefined;
  gNextObjectId = 1;
  sendMessage("Runtime.releaseObjectGroup", { objectGroup: ObjectGroup });
  sendMessage("Runtime.releaseObjectGroup", { objectGroup: "backtrace" });
}

function remoteObjectToProtocolId(remoteObject) {
  assert(remoteObject.objectId);

  const existing = gObjectIdToProtocolId.get(remoteObject.objectId);
  if (existing) {
    return existing;
  }

  const protocolObjectId = (gNextObjectId++).toString();
  gObjectIdToProtocolId.set(remoteObject.objectId, protocolObjectId);
  gProtocolIdToObject.set(protocolObjectId, remoteObject);

  return protocolObjectId;
}

function protocolIdToRemoteObject(objectId) {
  const remoteObject = gProtocolIdToObject.get(objectId);
  assert(remoteObject);
  return remoteObject;
}

// The ID a client knows one of the page's objects by.
function registerPlainObject(plainObject) {
  const text = rr.wrapValue(plainObject);
  if (!text) {
    return undefined;
  }
  const remoteObject = JSON.parse(text);
  return remoteObject.objectId ? remoteObjectToProtocolId(remoteObject) : undefined;
}

// The page's object for an ID, or undefined when the ID isn't for one.
function getPlainObjectByProtocolId(objectId) {
  const remoteObject = gProtocolIdToObject.get(String(objectId));
  return remoteObject ? rr.unwrapObject(remoteObject.objectId) : undefined;
}

// Gives a description that has no object of the inspector's behind it an ID,
// or the ID of |plainObject| when that already has one.
function registerSyntheticObject(synthetic, plainObject) {
  let objectId = plainObject && registerPlainObject(plainObject);
  if (!objectId) {
    objectId = (gNextObjectId++).toString();
  }
  synthetic.objectId = objectId;
  gSyntheticObjects.set(objectId, synthetic);
  return objectId;
}

function parseOrNull(text) {
  return typeof text == "string" && text ? JSON.parse(text) : null;
}

function getDomBundle() {
  if (gDomBundle === undefined) {
    gDomBundle = null;
    if (rr.domBundle) {
      gDomBundle = rr.domBundle({
        registerPlainObject,
        getPlainObjectByRrpId: getPlainObjectByProtocolId,
        registerRrpPreview: registerSyntheticObject,
        log,
        layoutDom: rr.domLayout,
        fromJsIsBlinkObject: value => !!rr.domClassify(value),
        fromJsIsBlinkNodeObject: value => {
          const kind = rr.domClassify(value);
          return kind == "node" || kind == "element";
        },
        fromJsIsBlinkElementObject: value => rr.domClassify(value) == "element",
        fromJsIsBlinkCSSStyleDeclarationObject: value => rr.domClassify(value) == "style",
        fromJsGetBoxModel: node => parseOrNull(rr.domBoxModel(node)),
        fromJsGetMatchedStylesForElement: node => parseOrNull(rr.cssMatchedStyles(node)),
        fromJsCssGetStylesheetByCpdId: id => rr.cssStyleSheet(id),
        fromJsCollectEventListeners: node => rr.domEventListeners(node),
        fromJsDomPerformSearch: query => rr.domSearch(query),
        fromJsGetDevicePixelRatio: rr.devicePixelRatio,
      }) || null;
    }
  }
  return gDomBundle;
}

// Strings longer than this will be truncated when creating protocol values.
const MaxStringLength = 10000;

function remoteObjectToProtocolValue(obj) {
  switch (obj.type) {
    case "undefined":
      return {};
    case "string":
    case "number":
    case "boolean":
      if (obj.unserializableValue) {
        assert(obj.type == "number");
        return { unserializableNumber: obj.unserializableValue };
      }
      if (typeof obj.value == "string" && obj.value.length > MaxStringLength) {
        return { value: obj.value.substring(0, MaxStringLength) + "…" };
      }
      return { value: obj.value };
    case "bigint": {
      const str = obj.unserializableValue;
      assert(str);
      return { bigint: str.substring(0, str.length - 1) };
    }
    case "object":
    case "function": {
      if (!obj.objectId) {
        return { value: null };
      }
      const object = remoteObjectToProtocolId(obj);
      return { object };
    }
    case "symbol":
      return { symbol: obj.description };
    default:
      return { unavailable: true };
  }
}

function scopeToProtocolId(scope) {
  // Use the scope object's ID as the ID for the scope itself.
  const id = remoteObjectToProtocolId(scope.object);
  gProtocolIdToScope.set(id, scope);
  return id;
}

function protocolIdToScope(scopeId) {
  const scope = gProtocolIdToScope.get(scopeId);
  assert(scope);
  return scope;
}

// Logic for creating object previews for the record/replay protocol.


function createProtocolObject(objectId, level) {
  const synthetic = gSyntheticObjects.get(objectId);
  if (synthetic) {
    return {
      objectId,
      className: synthetic.className,
      preview: level != "none" ? synthetic.preview : undefined,
    };
  }
  const obj = protocolIdToRemoteObject(objectId);
  const className = obj.subtype == "proxy" ? "Proxy" : (obj.className || "Function");

  let preview;
  if (level != "none") {
    preview = new ProtocolObjectPreview(obj, level).fill();
  }

  // As Replay's Chromium 108 gives it: the object's persistent ID when it has
  // one, as a string. Reading it never makes one.
  const persistent = rr.persistentId(rr.unwrapObject(obj.objectId));
  const persistentId = persistent ? String(persistent) : undefined;

  return { objectId, persistentId, className, preview };
}

// Target limit for the number of items (properties etc.) to include in object
// previews before overflowing.
const MaxItems = {
  "noProperties": 0,

  // Note: this is higher than on gecko-dev because typed arrays don't render
  // properly in the devtools currently unless we include a minimum number of
  // properties. This would be nice to fix.
  "canOverflow": 10,

  "full": 1000,
};

function ProtocolObjectPreview(obj, level) {
  this.obj = obj;
  this.level = level;
  this.overflow = false;
  this.numItems = 0;
  this.extra = {};
}

ProtocolObjectPreview.prototype = {
  canAddItem(force) {
    if (!force && this.numItems >= MaxItems[this.level]) {
      this.overflow = true;
      return false;
    }
    this.numItems++;
    return true;
  },

  addProperty(property, force) {
    if (!this.canAddItem(force)) {
      return;
    }
    if (!this.properties) {
      this.properties = [];
    }
    this.properties.push(property);
  },

  // What a proxy wraps, how a promise stands, and for one of the page's DOM
  // objects what the page says of it.
  getExtraData(allProperties) {
    const internal = name => {
      const found = (allProperties.internalProperties || []).find(p => p.name == name);
      return found && found.value;
    };
    if (this.obj.subtype == "proxy") {
      const target = internal("[[Target]]");
      const handler = internal("[[Handler]]");
      if (target && handler) {
        return {
          proxyState: {
            target: remoteObjectToProtocolValue(target),
            handler: remoteObjectToProtocolValue(handler),
          },
        };
      }
      return undefined;
    }
    if (this.obj.subtype == "promise") {
      const state = internal("[[PromiseState]]");
      const value = internal("[[PromiseResult]]");
      const promiseState = { state: (state && state.value) || undefined };
      if (promiseState.state !== "pending" && value) {
        promiseState.value = remoteObjectToProtocolValue(value);
      }
      return { promiseState };
    }
    if (!rr.domClassify) {
      return undefined;
    }
    const plainObject = rr.unwrapObject(this.obj.objectId);
    const kind = rr.domClassify(plainObject);
    if (!kind) {
      return undefined;
    }
    const bundle = getDomBundle();
    if (!bundle) {
      return undefined;
    }
    if (kind == "style") {
      return { style: bundle.previewBlinkStyle(plainObject) };
    }
    return { node: bundle.previewBlinkNode(plainObject) };
  },

  addContainerEntry(entry) {
    if (!this.canAddItem()) {
      return;
    }
    if (!this.containerEntries) {
      this.containerEntries = [];
    }
    this.containerEntries.push(entry);
  },

  fill() {
    const allProperties = sendMessage("Runtime.getProperties", {
      objectId: this.obj.objectId,
      ownProperties: true,
      generatePreview: false,
    });
    const properties = allProperties.result;

    // Add class-specific data.
    const previewer = CustomPreviewers[this.obj.className];
    const requiredProperties = [];
    if (previewer) {
      for (const entry of previewer) {
        if (typeof entry == "string") {
          requiredProperties.push(entry);
        } else {
          entry.call(this, allProperties);
        }
      }
    }

    let prototype;
    for (const prop of properties) {
      if (prop.name == "__proto__") {
        prototype = prop;
      } else {
        const protocolProperty = createProtocolPropertyDescriptor(
          previewer === ErrorProperties ? errorStackAsValue(this.obj.objectId, prop) : prop
        );
        const force = requiredProperties.includes(prop.name);
        this.addProperty(protocolProperty, force);
      }
    }

    // The prototype is the [[Prototype]] internal property, which is there
    // whatever the prototype is. A __proto__ entry among the properties isn't
    // when the prototype is a proxy: listing stops there, so that its traps
    // aren't run.
    const internalPrototype = (allProperties.internalProperties || []).find(
      p => p.name == "[[Prototype]]");
    if (internalPrototype && internalPrototype.value &&
        internalPrototype.value.objectId) {
      prototype = internalPrototype;
    }
    let prototypeId;
    if (prototype && prototype.value && prototype.value.objectId) {
      prototypeId = remoteObjectToProtocolId(prototype.value);
    }
    Object.assign(this.extra, this.getExtraData(allProperties) || {});
    return {
      prototypeId,
      overflow: (this.overflow && this.level != "full") ? true : undefined,
      properties: this.properties,
      containerEntries: this.containerEntries,
      ...this.extra,
    };
  },
};

// Get a count from an object description like "Array(42)"
function getDescriptionCount(description) {
  const match = /\((\d+)\)/.exec(description || "");
  if (match) {
    return +match[1];
  }
}

function previewTypedArray() {
  // The typed array size isn't available from the object's own property
  // information, except by parsing the object description.
  const length = getDescriptionCount(this.obj.description);
  if (length !== undefined) {
    this.addProperty({ name: "length", value: length }, /* force */ true);
    // The length is a getter's value, which is where Replay's Chromium 108
    // reports it (it calls the native getter; the count in the description is
    // the same number).
    this.extra.getterValues = [{ name: "length", value: length }];
  }
}

function previewSetMap(allProperties) {
  if (!allProperties.internalProperties) {
    return;
  }

  const internal = allProperties.internalProperties.find(prop => prop.name == "[[Entries]]");
  if (!internal || !internal.value || !internal.value.objectId) {
    return;
  }

  // Get the container size from the length of the entries.
  const size = getDescriptionCount(internal.value.description);
  if (size !== undefined) {
    this.extra.containerEntryCount = size;
    if (["Set", "Map"].includes(this.obj.className)) {
      this.addProperty({ name: "size", value: size }, /* force */ true);
    }
  }

  const entries = sendMessage("Runtime.getProperties", {
    objectId: internal.value.objectId,
    ownProperties: true,
    generatePreview: false,
  }).result;

  for (const entry of entries) {
    if (entry.value.subtype == "internal#entry") {
      const entryProperties = sendMessage("Runtime.getProperties", {
        objectId: entry.value.objectId,
        ownProperties: true,
        generatePreview: false,
      }).result;
      const key = entryProperties.find(eprop => eprop.name == "key");
      const value = entryProperties.find(eprop => eprop.name == "value");
      if (value) {
        this.addContainerEntry({
          key: key ? remoteObjectToProtocolValue(key.value) : undefined,
          value: remoteObjectToProtocolValue(value.value),
        });
      }
    }
    if (this.overflow) {
      break;
    }
  }
}

function previewRegExp() {
  this.extra.regexpString = this.obj.description;
}

function previewDate() {
  const dateTime = Date.parse(this.obj.description);
  if (!Number.isNaN(dateTime)) {
    this.extra.dateTime = dateTime;
  }
}

function previewError() {
  // A getter's value, as in Replay's Chromium 108: an Error has no name of its
  // own.
  this.extra.getterValues = [
    ...(this.extra.getterValues || []),
    { name: "name", value: this.obj.className },
  ];
}

// An Error's own stack accessor, as the data property it was in V8 10.8, when
// the stack has been formatted. See errorFormattedStack in replay-glue.cc.
function errorStackAsValue(objectId, prop) {
  if (prop.name != "stack" || prop.value || !prop.get || !rr.errorFormattedStack) {
    return prop;
  }
  const stack = rr.errorFormattedStack(rr.unwrapObject(objectId));
  if (typeof stack != "string") {
    return prop;
  }
  return {
    name: "stack",
    value: { type: "string", value: stack },
    writable: true,
    configurable: true,
    enumerable: false,
  };
}

const ErrorProperties = [
  "message",
  "stack",
  previewError,
];

function previewFunction(allProperties) {
  const nameProperty = allProperties.result.find(prop => prop.name == "name");
  if (nameProperty) {
    this.extra.functionName = nameProperty.value.value;
  }

  const locationProperty = allProperties.internalProperties.find(
    prop => prop.name == "[[FunctionLocation]]"
  );
  if (locationProperty) {
    this.extra.functionLocation = createProtocolLocation(locationProperty.value.value);
  }
}

const CustomPreviewers = {
  Array: ["length"],
  Int8Array: [previewTypedArray],
  Uint8Array: [previewTypedArray],
  Uint8ClampedArray: [previewTypedArray],
  Int16Array: [previewTypedArray],
  Uint16Array: [previewTypedArray],
  Int32Array: [previewTypedArray],
  Uint32Array: [previewTypedArray],
  Float32Array: [previewTypedArray],
  Float64Array: [previewTypedArray],
  BigInt64Array: [previewTypedArray],
  BigUint64Array: [previewTypedArray],
  Map: [previewSetMap],
  WeakMap: [previewSetMap],
  Set: [previewSetMap],
  WeakSet: [previewSetMap],
  RegExp: [previewRegExp],
  Date: [previewDate],
  Error: ErrorProperties,
  EvalError: ErrorProperties,
  RangeError: ErrorProperties,
  ReferenceError: ErrorProperties,
  SyntaxError: ErrorProperties,
  TypeError: ErrorProperties,
  URIError: ErrorProperties,
  Function: [previewFunction],
};

function createProtocolPropertyDescriptor(desc) {
  const { name, value, writable, get, set, configurable, enumerable, symbol } = desc;

  const rv = value ? remoteObjectToProtocolValue(value) : {};
  rv.name = name;

  let flags = 0;
  if (writable) {
    flags |= 1;
  }
  if (configurable) {
    flags |= 2;
  }
  if (enumerable) {
    flags |= 4;
  }
  if (flags != 7) {
    rv.flags = flags;
  }

  if (get && get.objectId) {
    rv.get = remoteObjectToProtocolId(get);
  }
  if (set && set.objectId) {
    rv.set = remoteObjectToProtocolId(set);
  }

  if (symbol) {
    rv.isSymbol = true;
  }

  return rv;
}

function createProtocolLocation(location) {
  if (!location) {
    return undefined;
  }
  const { scriptId, lineNumber, columnNumber } = location;
  return [{
    sourceId: scriptId,
    // CDP line numbers are 0-indexed, while RRP line numbers are 1-indexed.
    line: lineNumber + 1,
    column: columnNumber,
  }];
}

function createProtocolFrame(frameId, frame, topmost) {
  // CDP call frames don't provide detailed type information.
  const type = frame.functionName ? "call" : "global";

  // Stopped where the top frame returns, there's what it returns.
  let returnValue;
  if (topmost && rr.hasReturnValue()) {
    const text = rr.wrapValue(rr.returnValue());
    if (text) {
      returnValue = remoteObjectToProtocolValue(JSON.parse(text));
    }
  }

  return {
    frameId,
    type,
    functionName: frame.functionName || undefined,
    functionLocation: createProtocolLocation(frame.functionLocation),
    location: createProtocolLocation(frame.location),
    scopeChain: frame.scopeChain.map(scopeToProtocolId),
    this: remoteObjectToProtocolValue(frame.this),
    returnValue,
  };
}

function createProtocolScope(scopeId) {
  const scope = protocolIdToScope(scopeId);

  let type;
  switch (scope.type) {
    case "global":
      type = "global";
      break;
    case "with":
      type = "with";
      break;
    default:
      type = scope.name ? "function" : "block";
      break;
  }

  let object, bindings;
  if (type == "global" || type == "with") {
    object = remoteObjectToProtocolId(scope.object);
  } else {
    bindings = [];

    const properties = sendMessage("Runtime.getProperties", {
      objectId: scope.object.objectId,
      ownProperties: true,
      generatePreview: false,
    }).result;
    for (const { name, value } of properties) {
      // CDP omits the value of a binding it can't describe, such as a dead
      // register an async function restored when resuming in the interpreter.
      const converted = value
        ? remoteObjectToProtocolValue(value)
        : { unavailable: true };
      bindings.push({ ...converted, name });
    }
  }

  return {
    scopeId,
    type,
    object,
    functionName: scope.name || undefined,
    bindings,
  };
}

const CommandCallbacks = {
  "Target.getCurrentMessageContents": Target_getCurrentMessageContents,
  "Target.getSourceMapURL": () => ({}),
  "Target.getStepOffsets": () => ({}),
  "Target.topFrameLocation": Target_topFrameLocation,
  "Pause.evaluateInFrame": Pause_evaluateInFrame,
  "Pause.evaluateInGlobal": Pause_evaluateInGlobal,
  "Pause.getAllFrames": Pause_getAllFrames,
  "Pause.getExceptionValue": Pause_getExceptionValue,
  "Pause.getObjectPreview": Pause_getObjectPreview,
  "Pause.getObjectProperty": Pause_getObjectProperty,
  "Pause.getScope": Pause_getScope,
};

function commandCallback(method, paramsText) {
  let callback = CommandCallbacks[method];
  if (!callback && (method.startsWith("DOM.") || method.startsWith("CSS."))) {
    const bundle = getDomBundle();
    callback = bundle && bundle.commands[method];
  }
  if (!callback) {
    // The embedder has commands of its own, which it answers in the page.
    const answer = rr.pageCommand(method, paramsText || "{}");
    if (typeof answer === "string") {
      return answer;
    }
    log(`Missing command callback: ${method}`);
    return "{}";
  }
  try {
    return JSON.stringify(callback(JSON.parse(paramsText || "{}")) || {});
  } catch (e) {
    log(`Error: Command exception ${method} ${e}`);
    return JSON.stringify({ error: String(e) });
  }
}

const ConsoleLevels = new Set(["info", "warning", "error", "timeEnd"]);

// The console call that's happening now, which C++ describes as it's in V8's own
// console builtin rather than in a frame on the stack.
function Target_getCurrentMessageContents() {
  // An uncaught error, which the embedder reports, or a console call.
  const errorText = rr.currentPageError();
  if (errorText) {
    const error = JSON.parse(errorText);
    return {
      source: "PageError",
      level: "error",
      text: error.message,
      url: error.url || undefined,
      sourceId: error.scriptId > 0 ? String(error.scriptId) : undefined,
      line: error.line,
      column: error.column,
    };
  }
  const call = JSON.parse(rr.currentConsoleCall());
  let url, sourceId, line, column;
  const { frameLocations } = sendMessage("Debugger.getFrameLocations", {});
  for (const location of frameLocations) {
    if (!rr.ignoreScript(location.scriptId)) {
      sourceId = location.scriptId;
      line = location.lineNumber + 1;
      column = location.columnNumber;
      break;
    }
  }
  return {
    source: "ConsoleAPI",
    level: ConsoleLevels.has(call.level) ? call.level : "info",
    text: "",
    url,
    sourceId,
    line,
    column,
    argumentValues: call.args.map(remoteObjectToProtocolValue),
  };
}

function Target_topFrameLocation() {
  const { frameLocations } = sendMessage("Debugger.getFrameLocations", {});
  for (const location of frameLocations) {
    if (!rr.ignoreScript(location.scriptId)) {
      return { location: createProtocolLocation(location)[0] };
    }
  }
  return {};
}

// The raw call frames on the stack, without those in scripts being ignored.
function getStackFrames() {
  const { callFrames } = sendMessage("Debugger.getCallFrames", {});
  return callFrames.filter(frame => !rr.ignoreScript(frame.location.scriptId));
}

function buildProtocolResult({ result, exceptionDetails }) {
  const value = remoteObjectToProtocolValue(result);
  const protocolResult = { data: {} };
  if (exceptionDetails) {
    protocolResult.exception = value;
  } else {
    protocolResult.returned = value;
  }
  return { result: protocolResult };
}

function Pause_evaluateInFrame({ frameId, expression }) {
  const frames = getStackFrames();
  const index = +frameId;
  assert(index < frames.length);
  return buildProtocolResult(sendMessage("Debugger.evaluateOnCallFrame", {
    callFrameId: frames[index].callFrameId,
    expression,
    objectGroup: ObjectGroup,
  }));
}

function Pause_evaluateInGlobal({ expression }) {
  return buildProtocolResult(sendMessage("Runtime.evaluate", { expression, objectGroup: ObjectGroup }));
}

function Pause_getAllFrames() {
  const frames = getStackFrames().map((frame, index) =>
    createProtocolFrame(String(index), frame, index == 0)
  );
  return { frames: frames.map(f => f.frameId), data: { frames } };
}

function Pause_getExceptionValue() {
  const rv = sendMessage("Debugger.getPendingException", {});
  return { exception: remoteObjectToProtocolValue(rv.exception), data: {} };
}

function Pause_getObjectPreview({ object, level = "full" }) {
  return { data: { objects: [createProtocolObject(object, level)] } };
}

function Pause_getObjectProperty({ object, name }) {
  const obj = protocolIdToRemoteObject(object);
  return buildProtocolResult(sendMessage("Runtime.callFunctionOn", {
    functionDeclaration: `function() { return this[${JSON.stringify(name)}] }`,
    objectId: obj.objectId,
  }));
}

function Pause_getScope({ scope }) {
  return { data: { scopes: [createProtocolScope(scope)] } };
}

sendMessage("Runtime.enable", {});
sendMessage("Debugger.enable", {});

return { commandCallback, clearPauseDataCallback };
})
