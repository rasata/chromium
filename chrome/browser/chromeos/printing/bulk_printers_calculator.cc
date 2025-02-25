// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/printing/bulk_printers_calculator.h"

#include <set>

#include "base/bind.h"
#include "base/feature_list.h"
#include "base/json/json_reader.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/sequence_checker.h"
#include "base/sequenced_task_runner.h"
#include "base/stl_util.h"
#include "base/task/post_task.h"
#include "base/task_runner_util.h"
#include "base/threading/scoped_blocking_call.h"
#include "base/values.h"
#include "chrome/common/chrome_features.h"
#include "chromeos/printing/printer_translator.h"

namespace chromeos {

namespace {

constexpr int kMaxRecords = 20000;

// Represents a task scheduled to process in the Restrictions class.
struct TaskDataInternal {
  const unsigned task_id;  // unique ID in increasing order
  std::unordered_map<std::string, Printer> printers;  // resultant list (output)
  explicit TaskDataInternal(unsigned id) : task_id(id) {}
};

using PrinterCache = std::vector<std::unique_ptr<Printer>>;
using TaskData = std::unique_ptr<TaskDataInternal>;

// Parses |data|, a JSON blob, into a vector of Printers.  If |data| cannot be
// parsed, returns nullptr.  This is run off the UI thread as it could be very
// slow.
std::unique_ptr<PrinterCache> ParsePrinters(std::unique_ptr<std::string> data) {
  if (!data) {
    LOG(WARNING) << "Received null data";
    return nullptr;
  }
  int error_code = 0;
  int error_line = 0;

  // This could be really slow.
  base::ScopedBlockingCall scoped_blocking_call(FROM_HERE,
                                                base::BlockingType::MAY_BLOCK);
  std::unique_ptr<base::Value> json_blob =
      base::JSONReader::ReadAndReturnErrorDeprecated(
          *data, base::JSONParserOptions::JSON_PARSE_RFC, &error_code,
          nullptr /* error_msg_out */, &error_line);
  // It's not valid JSON.  Give up.
  if (!json_blob || !json_blob->is_list()) {
    LOG(WARNING) << "Failed to parse printers policy (" << error_code
                 << ") on line " << error_line;
    return nullptr;
  }

  const base::Value::ListStorage& printer_list = json_blob->GetList();
  if (printer_list.size() > kMaxRecords) {
    LOG(WARNING) << "Too many records in printers policy: "
                 << printer_list.size();
    return nullptr;
  }

  auto parsed_printers = std::make_unique<PrinterCache>();
  parsed_printers->reserve(printer_list.size());
  for (const base::Value& val : printer_list) {
    // TODO(skau): Convert to the new Value APIs.
    const base::DictionaryValue* printer_dict;
    if (!val.GetAsDictionary(&printer_dict)) {
      LOG(WARNING) << "Entry in printers policy skipped.  Not a dictionary.";
      continue;
    }

    auto printer = RecommendedPrinterToPrinter(*printer_dict);
    if (!printer) {
      LOG(WARNING) << "Failed to parse printer configuration.  Skipped.";
      continue;
    }
    parsed_printers->push_back(std::move(printer));
  }

  return parsed_printers;
}

// Computes the effective printer list using the access mode and
// blacklist/whitelist.  Methods are required to be sequenced.  This object is
// the owner of all the policy data. Methods updating the list of available
// printers take TaskData (see above) as |task_data| parameter and returned it.
class Restrictions {
 public:
  Restrictions() : printers_cache_(nullptr) {
    DETACH_FROM_SEQUENCE(sequence_checker_);
  }
  ~Restrictions() { DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_); }

  // Sets the printer cache using the policy blob |data|.
  TaskData SetData(TaskData task_data, std::unique_ptr<std::string> data) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    base::ScopedBlockingCall scoped_blocking_call(
        FROM_HERE, base::BlockingType::MAY_BLOCK);
    printers_cache_ = ParsePrinters(std::move(data));
    return ComputePrinters(std::move(task_data));
  }

  // Clear the printer cache.
  void ClearData() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    printers_cache_.reset();
  }

  // Sets the access mode to |mode|.
  TaskData UpdateAccessMode(TaskData task_data,
                            BulkPrintersCalculator::AccessMode mode) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    mode_ = mode;
    return ComputePrinters(std::move(task_data));
  }

  // Sets the blacklist to |blacklist|.
  TaskData UpdateBlacklist(TaskData task_data,
                           const std::vector<std::string>& blacklist) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    has_blacklist_ = true;
    blacklist_ = std::set<std::string>(blacklist.begin(), blacklist.end());
    return ComputePrinters(std::move(task_data));
  }

  // Sets the whitelist to |whitelist|.
  TaskData UpdateWhitelist(TaskData task_data,
                           const std::vector<std::string>& whitelist) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    has_whitelist_ = true;
    whitelist_ = std::set<std::string>(whitelist.begin(), whitelist.end());
    return ComputePrinters(std::move(task_data));
  }

 private:
  // Returns true if we have enough data to compute the effective printer list.
  bool IsReady() const {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (!printers_cache_) {
      return false;
    }
    switch (mode_) {
      case BulkPrintersCalculator::AccessMode::ALL_ACCESS:
        return true;
      case BulkPrintersCalculator::AccessMode::BLACKLIST_ONLY:
        return has_blacklist_;
      case BulkPrintersCalculator::AccessMode::WHITELIST_ONLY:
        return has_whitelist_;
      case BulkPrintersCalculator::AccessMode::UNSET:
        return false;
    }
    NOTREACHED();
    return false;
  }

  // Calculates resultant list of available printers.
  TaskData ComputePrinters(TaskData task_data) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

    if (!IsReady()) {
      return task_data;
    }

    switch (mode_) {
      case BulkPrintersCalculator::UNSET:
        NOTREACHED();
        break;
      case BulkPrintersCalculator::WHITELIST_ONLY:
        for (const auto& printer : *printers_cache_) {
          if (base::ContainsKey(whitelist_, printer->id())) {
            task_data->printers.insert({printer->id(), *printer});
          }
        }
        break;
      case BulkPrintersCalculator::BLACKLIST_ONLY:
        for (const auto& printer : *printers_cache_) {
          if (!base::ContainsKey(blacklist_, printer->id())) {
            task_data->printers.insert({printer->id(), *printer});
          }
        }
        break;
      case BulkPrintersCalculator::ALL_ACCESS:
        for (const auto& printer : *printers_cache_) {
          task_data->printers.insert({printer->id(), *printer});
        }
        break;
    }

    return task_data;
  }

  // Cache of the parsed printer configuration file.
  std::unique_ptr<PrinterCache> printers_cache_;
  // The type of restriction which is enforced.
  BulkPrintersCalculator::AccessMode mode_ = BulkPrintersCalculator::UNSET;
  // Blacklist: the list of ids which should not appear in the final list.
  bool has_blacklist_ = false;
  std::set<std::string> blacklist_;
  // Whitelist: the list of the only ids which should appear in the final list.
  bool has_whitelist_ = false;
  std::set<std::string> whitelist_;

  SEQUENCE_CHECKER(sequence_checker_);
  DISALLOW_COPY_AND_ASSIGN(Restrictions);
};

class BulkPrintersCalculatorImpl : public BulkPrintersCalculator {
 public:
  BulkPrintersCalculatorImpl()
      : restrictions_(std::make_unique<Restrictions>()),
        restrictions_runner_(base::CreateSequencedTaskRunnerWithTraits(
            {base::TaskPriority::BEST_EFFORT, base::MayBlock(),
             base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN})),
        weak_ptr_factory_(this) {}
  ~BulkPrintersCalculatorImpl() override {
    bool success =
        restrictions_runner_->DeleteSoon(FROM_HERE, std::move(restrictions_));
    if (!success) {
      LOG(WARNING) << "Unable to schedule deletion of policy object.";
    }
  }

  void AddObserver(Observer* observer) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    observers_.AddObserver(observer);
  }

  void RemoveObserver(Observer* observer) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    observers_.RemoveObserver(observer);
  }

  void ClearData() override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (!base::FeatureList::IsEnabled(features::kBulkPrinters)) {
      return;
    }
    data_is_set_ = false;
    last_processed_task_ = ++last_received_task_;
    printers_.clear();
    // Forward data to Restrictions to clear "Data".
    restrictions_runner_->PostTask(
        FROM_HERE, base::BindOnce(&Restrictions::ClearData,
                                  base::Unretained(restrictions_.get())));
    // Notify observers.
    for (auto& observer : observers_) {
      observer.OnPrintersChanged(this);
    }
  }

  void SetData(std::unique_ptr<std::string> data) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (!base::FeatureList::IsEnabled(features::kBulkPrinters)) {
      return;
    }
    data_is_set_ = true;
    TaskData task_data =
        std::make_unique<TaskDataInternal>(++last_received_task_);
    base::PostTaskAndReplyWithResult(
        restrictions_runner_.get(), FROM_HERE,
        base::BindOnce(&Restrictions::SetData,
                       base::Unretained(restrictions_.get()),
                       std::move(task_data), std::move(data)),
        base::BindOnce(&BulkPrintersCalculatorImpl::OnComputationComplete,
                       weak_ptr_factory_.GetWeakPtr()));
  }

  void SetAccessMode(AccessMode mode) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    TaskData task_data =
        std::make_unique<TaskDataInternal>(++last_received_task_);
    base::PostTaskAndReplyWithResult(
        restrictions_runner_.get(), FROM_HERE,
        base::BindOnce(&Restrictions::UpdateAccessMode,
                       base::Unretained(restrictions_.get()),
                       std::move(task_data), mode),
        base::BindOnce(&BulkPrintersCalculatorImpl::OnComputationComplete,
                       weak_ptr_factory_.GetWeakPtr()));
  }

  void SetBlacklist(const std::vector<std::string>& blacklist) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    TaskData task_data =
        std::make_unique<TaskDataInternal>(++last_received_task_);
    base::PostTaskAndReplyWithResult(
        restrictions_runner_.get(), FROM_HERE,
        base::BindOnce(&Restrictions::UpdateBlacklist,
                       base::Unretained(restrictions_.get()),
                       std::move(task_data), blacklist),
        base::BindOnce(&BulkPrintersCalculatorImpl::OnComputationComplete,
                       weak_ptr_factory_.GetWeakPtr()));
  }

  void SetWhitelist(const std::vector<std::string>& whitelist) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    TaskData task_data =
        std::make_unique<TaskDataInternal>(++last_received_task_);
    base::PostTaskAndReplyWithResult(
        restrictions_runner_.get(), FROM_HERE,
        base::BindOnce(&Restrictions::UpdateWhitelist,
                       base::Unretained(restrictions_.get()),
                       std::move(task_data), whitelist),
        base::BindOnce(&BulkPrintersCalculatorImpl::OnComputationComplete,
                       weak_ptr_factory_.GetWeakPtr()));
  }

  bool IsDataPolicySet() const override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    return data_is_set_;
  }

  bool IsComplete() const override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    return (last_processed_task_ == last_received_task_);
  }

  const std::unordered_map<std::string, Printer>& GetPrinters() const override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    return printers_;
  }

 private:
  // Called on computation completion. |task_data| corresponds to finalized
  // task.
  void OnComputationComplete(TaskData task_data) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (!task_data || task_data->task_id <= last_processed_task_) {
      // The task is outdated (ClearData() was called in the meantime).
      return;
    }
    last_processed_task_ = task_data->task_id;
    if (last_processed_task_ < last_received_task_ && printers_.empty() &&
        task_data->printers.empty()) {
      // No changes in the object's state.
      return;
    }
    printers_.swap(task_data->printers);
    task_data.reset();
    // Notifies observers about changes.
    for (auto& observer : observers_) {
      observer.OnPrintersChanged(this);
    }
  }

  // Holds the blacklist and whitelist.  Computes the effective printer list.
  std::unique_ptr<Restrictions> restrictions_;
  // Off UI sequence for computing the printer view.
  scoped_refptr<base::SequencedTaskRunner> restrictions_runner_;

  // True if printers_ is based on a current policy.
  bool data_is_set_ = false;
  // Id of the last scheduled task.
  unsigned last_received_task_ = 0;
  // Id of the last completed task.
  unsigned last_processed_task_ = 0;
  // The computed set of printers.
  std::unordered_map<std::string, Printer> printers_;

  base::ObserverList<BulkPrintersCalculator::Observer>::Unchecked observers_;
  SEQUENCE_CHECKER(sequence_checker_);
  DISALLOW_COPY_AND_ASSIGN(BulkPrintersCalculatorImpl);
  base::WeakPtrFactory<BulkPrintersCalculatorImpl> weak_ptr_factory_;
};

}  // namespace

// static
std::unique_ptr<BulkPrintersCalculator> BulkPrintersCalculator::Create() {
  return std::make_unique<BulkPrintersCalculatorImpl>();
}

}  // namespace chromeos
