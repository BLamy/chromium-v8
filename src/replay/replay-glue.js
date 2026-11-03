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

function clearPauseDataCallback() {
  gProtocolIdToObject.clear();
  gObjectIdToProtocolId.clear();
  gProtocolIdToScope.clear();
  gNextObjectId = 1;
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
  const obj = protocolIdToRemoteObject(objectId);
  const className = obj.subtype == "proxy" ? "Proxy" : (obj.className || "Function");

  let preview;
  if (level != "none") {
    preview = new ProtocolObjectPreview(obj, level).fill();
  }

  return { objectId, className, preview };
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
        const protocolProperty = createProtocolPropertyDescriptor(prop);
        const force = requiredProperties.includes(prop.name);
        this.addProperty(protocolProperty, force);
      }
    }

    let prototypeId;
    if (prototype && prototype.value && prototype.value.objectId) {
      prototypeId = remoteObjectToProtocolId(prototype.value);
    }
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
  this.addProperty({ name: "name", value: this.obj.className }, /* force */ true);
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

function createProtocolFrame(frameId, frame) {
  // CDP call frames don't provide detailed type information.
  const type = frame.functionName ? "call" : "global";

  return {
    frameId,
    type,
    functionName: frame.functionName || undefined,
    functionLocation: createProtocolLocation(frame.functionLocation),
    location: createProtocolLocation(frame.location),
    scopeChain: frame.scopeChain.map(scopeToProtocolId),
    this: remoteObjectToProtocolValue(frame.this),
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
  const callback = CommandCallbacks[method];
  if (!callback) {
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
  }));
}

function Pause_evaluateInGlobal({ expression }) {
  return buildProtocolResult(sendMessage("Runtime.evaluate", { expression }));
}

function Pause_getAllFrames() {
  const frames = getStackFrames().map((frame, index) =>
    createProtocolFrame(String(index), frame)
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
