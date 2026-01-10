#pragma once
#include <string>
#include <vector>
#include <iostream>
#include <memory>
#include <array>
#include <sstream>
#include <cstdlib>
#include <regex>

namespace api {

struct Job {
    std::string id;
    std::string name;
    std::string entry_name;
};

struct NodeAllocation {
    std::string node_name;
    std::vector<int> allocated_cores;
    int allocated_gpus;
    int total_cores;
    int total_gpus;
};

struct DetailedJob {
    int nodes = 0;

    std::string id;
    std::string name;
    std::string entry_name;
    std::string submitTime;
    std::string maxTime;
    std::string partition;
    std::string status;
    std::string constraints;

    std::vector<NodeAllocation> node_allocations;
};

class slurm {
private:
    static inline std::string exec(const std::string& cmd) {
        std::array<char, 128> buffer;
        std::string result;

        std::unique_ptr<FILE, int(*)(FILE*)> pipe(
            popen(cmd.c_str(), "r"),
            static_cast<int(*)(FILE*)>(pclose)
        );
        if (!pipe) return "";
        while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
            result += buffer.data();
        }
        return result;
    }

    static inline std::vector<int> parseCpuIds(const std::string& cpu_ids_str) {
        std::vector<int> cpu_ids;
        std::regex re(R"((\d+)-(\d+)|(\d+))");
        auto begin = std::sregex_iterator(cpu_ids_str.begin(), cpu_ids_str.end(), re);
        auto end = std::sregex_iterator();

        for (auto it = begin; it != end; ++it) {
            if ((*it)[1].matched && (*it)[2].matched) {
                int start = std::stoi((*it)[1].str());
                int stop  = std::stoi((*it)[2].str());
                for (int i = start; i <= stop; ++i)
                    cpu_ids.push_back(i);
            } else if ((*it)[3].matched) {
                cpu_ids.push_back(std::stoi((*it)[3].str()));
            }
        }
        return cpu_ids;
    }

    static inline std::pair<int,int> getNodeInfo(const std::string& node_name) {
        std::string out = exec("scontrol show node " + node_name);
        std::regex cpu_re("CPUTot=(\\d+)");
        std::regex gpu_re("Gres=gpu:[^:]*:(\\d+)");

        int total_cores = 0;
        int total_gpus = 0;

        std::smatch m;
        if (std::regex_search(out, m, cpu_re)) total_cores = std::stoi(m[1]);
        if (std::regex_search(out, m, gpu_re)) total_gpus = std::stoi(m[1]);

        return {total_cores, total_gpus};
    }

    static inline std::vector<std::string> expandNodelist(const std::string& nodelist) {
        std::string cmd = "scontrol show hostnames " + nodelist + " 2>/dev/null";
        std::string out = exec(cmd);
        std::vector<std::string> nodes;
        std::istringstream iss(out);
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.empty()) nodes.push_back(line);
        }
        return nodes;
    }

public:
    static std::vector<Job> getUserJobs() {
        std::vector<Job> jobs;

        const char* user = std::getenv("USER");
        if (!user) user = "unknown";

        std::string cmd = "squeue -u " + std::string(user) + " -o \"%i %j\" --noheader";

        std::array<char, 256> buffer;
        std::unique_ptr<FILE, int(*)(FILE*)> pipe(
            popen(cmd.c_str(), "r"),
            static_cast<int(*)(FILE*)>(pclose)
        );

        if (!pipe) {
            std::cerr << "Failed to run squeue command\n";
            return jobs;
        }

        while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
            std::string line = buffer.data();
            if (line.empty()) continue;

            std::istringstream iss(line);
            Job job;
            iss >> job.id >> job.name;
            job.entry_name = (job.name + " (" + job.id + ")");

            if (!job.id.empty() && !job.name.empty()) {
                jobs.push_back(job);
            }
        }

        return jobs;
    }

    static DetailedJob getJobDetails(const std::string& job_id) {
        DetailedJob job;

        std::string sctrl = exec("scontrol show jobid -dd " + job_id);
        if (sctrl.empty()) return job;

        std::regex field_re(R"((\w+)=([^\s]+))");
        for (std::sregex_iterator it(sctrl.begin(), sctrl.end(), field_re), end; it != end; ++it) {
            std::string key = (*it)[1];
            std::string val = (*it)[2];
            if (key == "JobId") job.id = val;
            else if (key == "JobName") job.name = val;
            else if (key == "SubmitTime") job.submitTime = val;
            else if (key == "NumNodes") job.nodes = std::stoi(val);
            else if (key == "TimeLimit") job.maxTime = val;
            else if (key == "Partition") job.partition = val;
            else if (key == "JobState") job.status = val;
            else if (key == "Features") job.constraints = val;
        }

        std::regex nodes_re(R"(^\s*Nodes=([^\s]+))", std::regex_constants::multiline);
        std::smatch m;

        if (std::regex_search(sctrl, m, nodes_re)) {
            auto node_str = m[1].str();
            auto nodes = expandNodelist(node_str);

            std::regex cpu_re(R"(CPU_IDs=([^\s]+))");
            std::smatch cpu_m;
            if (!std::regex_search(sctrl, cpu_m, cpu_re)) return job;
            auto cpu_str = cpu_m[1].str();

            std::vector<std::string> cpu_groups;
            std::stringstream ss(cpu_str);
            std::string group;
            while (std::getline(ss, group, ',')) cpu_groups.push_back(group);

            auto cpu_ids = parseCpuIds(cpu_str);
            int nodes_count = nodes.size();
            int cpus_per_node = cpu_ids.size() / nodes_count;
            int group_index = 0;

            std::regex gpu_re(R"(GRES=([^\s]+))");
            std::smatch gpu_m;
            int allocated_gpus = 0;
            
            if (std::regex_search(sctrl, gpu_m, gpu_re)) {
                std::string gres = gpu_m[1].str();
                std::regex gpunum(R"(gpu:[^:]*:(\d+))");
                std::smatch gm;
                if (std::regex_search(gres, gm, gpunum))
                    allocated_gpus = std::stoi(gm[1].str());
            }

            for (auto& n : nodes) {
                NodeAllocation na;
                na.node_name = n;
                
                auto [total_cores, total_gpus] = getNodeInfo(n);
                
                na.total_cores = total_cores;
                na.total_gpus = total_gpus;
                na.allocated_gpus = allocated_gpus / nodes.size();

                na.allocated_cores.clear();
                for (int i = 0; i < cpus_per_node; ++i) {
                    auto& g = cpu_ids[(group_index * cpus_per_node) + i];
                    na.allocated_cores.push_back(g);
                }

                group_index++;
                job.node_allocations.push_back(na);
            }
        }

        return job;
    }

};

}
