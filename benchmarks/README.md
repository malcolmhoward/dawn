# Retrieval Benchmarks

Measures DAWN's memory retrieval quality against three published benchmarks,
using the same datasets and metrics that other memory systems report on.

## Architecture

A C binary (`bench_retrieval`) exercises DAWN's real embedding engine and
document search scoring. A Python script (`run_benchmark.py`) loads benchmark
datasets, drives the C binary via JSON-lines on stdin/stdout, and computes
metrics. The binary uses an in-memory SQLite database — it never touches
DAWN's production data.

## Build

```bash
cmake --preset debug
make -C build-debug bench_retrieval
```

## Datasets

### LongMemEval (500 questions)

Source: [xiaowu0162/longmemeval-cleaned](https://huggingface.co/datasets/xiaowu0162/longmemeval-cleaned) on HuggingFace.

```bash
mkdir -p ~/datasets/longmemeval
curl -fsSL -o ~/datasets/longmemeval/longmemeval_s_cleaned.json \
  https://huggingface.co/datasets/xiaowu0162/longmemeval-cleaned/resolve/main/longmemeval_s_cleaned.json
```

### LoCoMo (10 conversations, ~2000 QA pairs)

Source: [snap-research/locomo](https://github.com/snap-research/locomo) on GitHub.

```bash
git clone --depth 1 https://github.com/snap-research/locomo.git ~/datasets/locomo
```

### ConvoMem (75K+ QA pairs)

Source: [Salesforce/ConvoMem](https://huggingface.co/datasets/Salesforce/ConvoMem) on HuggingFace.

```bash
mkdir -p ~/datasets/convomem
curl -fsSL -o ~/datasets/convomem/user_evidence_sample.json \
  "https://huggingface.co/datasets/Salesforce/ConvoMem/resolve/main/core_benchmark/evidence_questions/user_evidence/1_evidence/0050e213-5032-42a0-8041-b5eef2f8ab91_Telemarketer.json"
```

For the full dataset (all 6 categories), use `huggingface-cli`:

```bash
huggingface-cli download Salesforce/ConvoMem --repo-type dataset --local-dir ~/datasets/convomem
```

## Running

### LongMemEval

```bash
# Full run (500 questions, ~42 min with ONNX on Jetson)
python3 benchmarks/run_benchmark.py \
    --binary ./build-debug/tests/bench_retrieval \
    --benchmark longmemeval \
    --dataset ~/datasets/longmemeval/longmemeval_s_cleaned.json

# Quick test (50 questions, ~4 min)
python3 benchmarks/run_benchmark.py \
    --binary ./build-debug/tests/bench_retrieval \
    --benchmark longmemeval \
    --dataset ~/datasets/longmemeval/longmemeval_s_cleaned.json \
    --limit 50
```

### LoCoMo

```bash
python3 benchmarks/run_benchmark.py \
    --binary ./build-debug/tests/bench_retrieval \
    --benchmark locomo \
    --dataset ~/datasets/locomo/data/locomo10.json
```

### ConvoMem

```bash
python3 benchmarks/run_benchmark.py \
    --binary ./build-debug/tests/bench_retrieval \
    --benchmark convomem \
    --dataset ~/datasets/convomem/user_evidence_sample.json \
    --limit 100
```

## Options

### Embedding provider

The default provider is ONNX (all-MiniLM-L6-v2, 384 dims), which matches
the model used by most published benchmarks. For faster runs, use Ollama
with the same model on GPU:

```bash
ollama pull all-minilm
python3 benchmarks/run_benchmark.py \
    --binary ./build-debug/tests/bench_retrieval \
    --benchmark longmemeval \
    --dataset ~/datasets/longmemeval/longmemeval_s_cleaned.json \
    --provider ollama --model all-minilm --endpoint http://localhost:11434
```

### Raw mode

By default, DAWN uses hybrid scoring (cosine similarity + keyword boosting).
To benchmark raw cosine similarity only (no keyword boosting, for baseline
comparison against published results), add `--raw`:

```bash
python3 benchmarks/run_benchmark.py \
    --binary ./build-debug/tests/bench_retrieval \
    --benchmark longmemeval \
    --dataset ~/datasets/longmemeval/longmemeval_s_cleaned.json \
    --raw
```

### Save results

```bash
python3 benchmarks/run_benchmark.py \
    --binary ./build-debug/tests/bench_retrieval \
    --benchmark longmemeval \
    --dataset ~/datasets/longmemeval/longmemeval_s_cleaned.json \
    --output results.json
```

## Metrics

| Benchmark | Metric | Description |
|-----------|--------|-------------|
| LongMemEval | Recall@K | Did the correct session appear in the top K results? |
| LongMemEval | NDCG@K | Position-weighted relevance (higher = correct answer ranked earlier) |
| LoCoMo | Avg Recall | Fraction of evidence dialog IDs found in top 10 |
| ConvoMem | Avg Recall | Fraction of evidence messages found via substring match in top 10 |
