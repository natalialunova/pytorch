#include <torch/csrc/jit/passes/quantization.h>

#include <torch/csrc/jit/ir.h>
#include <torch/csrc/jit/node_hashing.h>
#include <torch/csrc/jit/operator.h>
#include <torch/csrc/jit/passes/alias_analysis.h>

#include <stack>

namespace torch {
namespace jit {
namespace {
// QuantizerUtils

void insertNodeHelper(Node* new_n, Node* ins_node, bool insert_after) {
  AT_ASSERT(ins_node != nullptr);
  AT_ASSERT(new_n != nullptr);
  // True: After insertion point, False: Before insertion point
  if (insert_after) {
    new_n->insertAfter(ins_node);
  } else {
    new_n->insertBefore(ins_node);
  }
}

bool checkIfNodeQuantizable(Node* n) {
  AT_ASSERT(n != nullptr);
  // This is map for quantizable nodes. It will be expanded in future to
  // support more ops and patterns.
  static const OperatorSet quantnodeLookup =
   {
     "aten::conv2d(Tensor input, Tensor weight, Tensor? bias=None, int[2] \
stride=1, int[2] padding=0, int[2] dilation=1, int groups=1) -> Tensor",
     "aten::relu(Tensor self) -> Tensor",
     "aten::_convolution(Tensor input, Tensor weight, Tensor? bias, int[] \
stride, int[] padding, int[] dilation, bool transposed, int[] output_padding, \
int groups, bool benchmark, bool deterministic, bool cudnn_enabled) -> Tensor"
   };
  return quantnodeLookup.find(n);
}

void insertQuantNodeParams(Node* quant, float scale, int zero_point) {
  WithInsertPoint ins(quant);
  Value* scale_v = quant->owningGraph()->insertConstant(scale);
  Value* zeropoint_v = quant->owningGraph()->insertConstant(zero_point);
  quant->addInput(scale_v);
  quant->addInput(zeropoint_v);
}

// Create Quant Node
Node* createQuantNode(Value* v, Node* n) {
  Node* quant =
      n->owningGraph()->create(at::Symbol::fromQualString(
        "aten::quantize_linear"));
  AT_ASSERTM(quant != nullptr, "Failed to create quant node");
  quant->output()->setUniqueName(v->uniqueName() + ".quant");
  quant->setScope(n->scope());
  return quant;
}

// Create Dequant node
Node* createDeQuantNode(Value* v, Node* n) {
  Node* dequant =
      n->owningGraph()->create(at::Symbol::fromQualString("aten::dequantize"));
  AT_ASSERTM(dequant != nullptr, "Failed to create dequant node");
  dequant->output()->setUniqueName(v->uniqueName() + ".dequant");
  dequant->setScope(n->scope());
  return dequant;
}

// Insert Quant-Dequant node pair for quantizable node outputs
void addQuantDeQuantNodes(Value* v,
  std::tuple<std::string, float, int>& qparam) {
  AT_ASSERT(v != nullptr);
  Node* n = v->node();
  Node* quant = createQuantNode(v, n);
  Node* dequant = createDeQuantNode(v, n);

  // Add quant-dequant nodes and replace for all uses of Value
  quant->insertAfter(n);
  dequant->insertAfter(quant);
  v->replaceAllUsesWith(dequant->output());

  // Attach inputs to quant and dequant nodes
  quant->addInput(v);

  insertQuantNodeParams(quant, std::get<1>(qparam),
    std::get<2>(qparam));
  dequant->addInput(quant->output());
}

// Insert Quant-Dequant node pair for specific input to node n
void addQuantDeQuantNodesForInput(Value* v, Node* n,
  std::tuple<std::string, float, int>& qparam) {
  AT_ASSERT(v != nullptr);
  AT_ASSERT(n != nullptr);
  Node* quant = createQuantNode(v, n);
  Node* dequant = createDeQuantNode(v, n);

  // Insert the quant-dequant node for the V->N
  // pair which is identified as quantizable during
  // graph iteration
  dequant->insertBefore(n);
  quant->insertBefore(dequant);
  n->replaceInputWith(v, dequant->output());

  // Attach inputs to quant and dequant nodes
  quant->addInput(v);
  // Default Quant Params <Scale:1.0, ZeroPoint:0>
  insertQuantNodeParams(quant, std::get<1>(qparam),
    std::get<2>(qparam));
  dequant->addInput(quant->output());
}

template <typename... ArgT>
bool matchQParamDictKeytoObserver(Node* n,
  std::unordered_map<std::string, std::tuple<ArgT...>>& qparam_dict,
  std::unordered_map<Value*, std::tuple<ArgT...>>& qparam_value_dict) {
  if (n->kind() != prim::PythonOp) {
    return false;
  }
  Value* vname = n->inputs()[1];
  IValue valuekey = toIValue(vname).value();
  Value* v = n->inputs()[0];
  if (qparam_dict.count(valuekey.toStringRef()) == 0) {
    return false;
  }
  // This is observer node for Value v
  qparam_value_dict.emplace(v, qparam_dict[valuekey.toStringRef()]);
  return true;
}

} // namespace

// PyBind APIs
void PropagateQuantInfo(std::shared_ptr<Graph>& graph) {
  throw std::runtime_error("Pass not implemented yet!");
}

static void addObserverFor(Value* v, Node* original_observer_node,
  std::pair<Node*, bool> insert_info) {
  Node* def = insert_info.first;
  AT_ASSERT(def != nullptr);
  WithInsertPoint ins(def);

  // We need to pass the value name to observer function - create a constant
  // holding this name.
  Value* vname = def->owningGraph()->insertConstant(v->uniqueName());

  // Create a new observer node. We just need to clone the original one.
  Node* observerNode =
      def->owningGraph()
          ->createClone(
              &*original_observer_node, [&](Value* v) { return v; }, false);
  insertNodeHelper(observerNode, def, insert_info.second);
  // Set the type and the name of the output of the new observer node. It will
  // be used instead of the original value v.
  Value* observedValue = observerNode->addOutput();
  observedValue->setType(v->type());
  observedValue->setUniqueName(v->uniqueName() + ".observed");

  // Now we can add the inputs.
  observerNode->addInput(v);
  observerNode->addInput(vname);
}

static bool outputsNeedToBeObserved(Node* n) {
  return n->kind() != prim::Constant && n->kind() != prim::PythonOp;
}

void InsertObserverNodes(script::Method* method,
  std::unordered_map<std::string, Node*>& observerNodeDict) {
  AT_ASSERT(method != nullptr);
  auto graph = method->graph();
  AT_ASSERT(graph != nullptr);
  // For storing all values that need to be instrumented with an observer call.
  std::vector<Value*> values_to_observe;

  // For traversing all blocks in the graph including subblocks.
  std::stack<Block*> blocks_to_visit;

  // Observer nodes
  Node* activation_observer = observerNodeDict.count("activation")
    ? observerNodeDict["activation"] : nullptr;
  Node* param_observer = observerNodeDict.count("param")
    ? observerNodeDict["param"] : nullptr;

  // Add observer for prim::Param nodes
  auto inputVals = graph->inputs();
  int inputlength = inputVals.size();
  // Module params get appended after external inputs
  int param_start_index = inputlength - method->initial_ivalues().size();
  AT_ASSERT(param_start_index >= 0);
  // Insert point is the beginning of graph node
  Node* insert_node = *graph->nodes().begin();
  for (auto idx = 0; idx < inputlength; ++idx) {
    // This distinguish the model param from external data
    // to insert correct observer
    Node* observer = (idx < param_start_index) ?
      activation_observer : param_observer;
    if (observer == nullptr) {
      continue;
    }
    auto& v = inputVals[idx];
    if (v->type()->isSubtypeOf(TensorType::get())) {
      addObserverFor(v, observer,
        std::make_pair(insert_node, false));
    }
  }

  blocks_to_visit.push(graph->block());
  while (!blocks_to_visit.empty()) {
    Block* b = blocks_to_visit.top();
    blocks_to_visit.pop();

    for (Node* n : b->nodes()) {
      // Skip nodes that we don't need to observe, e.g. 'prim::Constant'.
      if (!outputsNeedToBeObserved(n)) {
        continue;
      }

      // Schedule subblocks (if any) for visiting.
      for (Block* subblock : n->blocks()) {
        blocks_to_visit.push(subblock);
      }

      // Activations not observed
      if (!activation_observer) {
        continue;
      }
      // Record all outputs in the values_to_observe - we'll later add observers
      // for all values from it.
      for (Value* v : n->outputs()) {
        values_to_observe.push_back(v);
      }
    }
  }

  // Actually add observer nodes for activations.
  for (Value* v : values_to_observe) {
    if (v->type()->isSubtypeOf(TensorType::get())) {
      addObserverFor(v, activation_observer,
        std::make_pair(v->node(), true));
    }
  }
}

void InsertQuantDequantNodes(std::shared_ptr<Graph>& graph,
  std::unordered_map<std::string, std::tuple<std::string,
  float, int>>& qparam_dict) {
  std::stack<Block*> blocks_to_visit;
  blocks_to_visit.push(graph->block());
  // For storing quantizable values - node pairs that are external
  // or intermediate inputs to quantizable nodes
  std::vector<std::pair<Value*, Node*>> quantInputs;
  // For storing quantizable values that are output of quantizable nodes
  // Since same value can go to multiple nodes, we use set so that
  // we insert quant-dequant node pairs for value only once
  std::vector<Value*> quantOutputs;
  std::unordered_set<Value*> valueLookup;

  // Observer nodes to remove from graph
  std::vector<Node*> nodes_to_remove;

  // Create value:qparam dict which guarantees no
  // name conflict between passes
  std::unordered_map<Value*,
    std::tuple<std::string, float, int>> qparam_value_dict;

  while (!blocks_to_visit.empty()) {
    Block* b = blocks_to_visit.top();
    blocks_to_visit.pop();

    for (Node* n : b->nodes()) {
      // Schedule the sub blocks
      for (Block* subblock : n->blocks()) {
        blocks_to_visit.push(subblock);
      }

      if (matchQParamDictKeytoObserver<std::string, float, int>
        (n, qparam_dict, qparam_value_dict)) {
        // This is the observer node. Mark it and the second input
        // constant nod for deletion.
        nodes_to_remove.emplace_back(n);
        nodes_to_remove.emplace_back(n->inputs()[1]->node());
        continue;
      }

      // We iterate over node inputs to identify which Values
      // need to be quantized depending on node type
      for (auto &v : n->inputs()) {
        if (!v->type()->isSubtypeOf(TensorType::get())) {
          // Skip quantization for non tensors
          continue;
        }

        if (checkIfNodeQuantizable(v->node())) {
          // Goal of this iteration is to identify the parent node for V
          // that is quantizable and replace all uses of Value with
          // quant-dequant output. Usage of set helps adding single
          // q-dq nodes for all V->users
          // Example N1 -> (V1 -> (N2), V2 -> (N3))
          //         N1 is quantizable node. So we insert quant-dequant
          //         nodes for all outputs of N1 (V1, V2) once
          if (!valueLookup.count(v)) {
            valueLookup.emplace(v);
            quantOutputs.emplace_back(v);
          }
        } else if (checkIfNodeQuantizable(n)) {
          // Goal of this iteration is to identify nodes that are
          // quantizable but input value originate from non quantizable
          // node. This requires selectively inserting q-dq nodes for
          // inputs into node N(V, N pair) because parent node might
          // also have input into other non quantizable nodes
          // Example : N1(prim::Param) -> (V1 -> (N4, N5), V2 -> (N6, N7), V3)
          //           N1 is not quantizable node but N4 and N7 are
          //           quantizable nodes. So we add the (V1, N4) and
          //           (V2, N7) as insertion points for quant-dequant nodes
          quantInputs.emplace_back(v, n);
        }
      }
    } // End Loop for nodes within block

    // Since we are iterating node inputs only above, we need to iterate
    // over block outputs values and if they originate from quantizable
    // node, we push to quantOutputs set
    auto outputVals = b->outputs();
    for (auto& v : outputVals) {
      if (checkIfNodeQuantizable(v->node()) &&
        v->type()->isSubtypeOf(TensorType::get())) {
        quantOutputs.emplace_back(v);
      }
    } //end for
  } // end Block traversal

  // Destory Observer Nodes
  for (auto& n : nodes_to_remove) {
    n->destroy();
  }

  // Insert the quant-dequant pair for values output from quantizable nodes
  for (auto& ele : quantOutputs) {
    if (qparam_value_dict.count(ele) != 0) {
      addQuantDeQuantNodes(ele, qparam_value_dict[ele]);
    }
  }

  // Insert the quant-dequant pair for values inputs to quantizable nodes
  for (auto& ele : quantInputs) {
    if (qparam_value_dict.count(ele.first) != 0) {
      addQuantDeQuantNodesForInput(ele.first,
        ele.second, qparam_value_dict[ele.first]);
    }
  }
}

void QuantLinting(std::shared_ptr<Graph>& graph) {
  throw std::runtime_error("Pass not implemented yet!");
}

void FoldQuantNodesIntoInputsOutputs(std::shared_ptr<Graph>& graph) {
  throw std::runtime_error("Pass not implemented yet!");
}

} // namespace jit
} // namespace torch
