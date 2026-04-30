#!/usr/bin/env php
<?php
/**
 * CodeQL Security Analysis Script
 * Cross-platform tool for analyzing C/C++ code with CodeQL
 */

class CodeQLAnalyzer {
    private string $codeqlPath;
    private string $projectRoot;
    private string $dbName = 'codeql-db';

    public function __construct() {
        $this->projectRoot = __DIR__;
        $this->detectCodeQL();
    }

    private function detectCodeQL(): void {
        $possiblePaths = [
            'e:\php\codeql-bundle-win64\codeql\codeql.exe',
            'e:\php\codeql-bundle-win64\codeql\codeql',
            '/usr/local/bin/codeql',
            getenv('HOME') . '/codeql/codeql',
        ];

        foreach ($possiblePaths as $path) {
            if (file_exists($path)) {
                $this->codeqlPath = $path;
                $this->log("✓ Found CodeQL at: {$path}");
                return;
            }
        }

        $which = PHP_OS_FAMILY === 'Windows' ? 'where' : 'which';
        exec("{$which} codeql 2>&1", $output, $code);
        if ($code === 0 && !empty($output[0])) {
            $this->codeqlPath = 'codeql';
            $this->log("✓ Found CodeQL in PATH");
            return;
        }

        $this->error("CodeQL not found! Please install or set path.");
    }

    private function log(string $message): void {
        echo "[" . date('H:i:s') . "] {$message}\n";
    }

    private function error(string $message): never {
        echo "\n❌ ERROR: {$message}\n\n";
        exit(1);
    }

    private function exec(string $command): array {
        $this->log("Running: {$command}");
        exec($command . ' 2>&1', $output, $code);
        return ['output' => $output, 'code' => $code];
    }

    public function createDatabase(?string $buildCommand = null): void {
        $this->log("Step 1: Creating CodeQL database...");

        if (is_dir($this->dbName)) {
            $this->log("Removing old database...");
            $this->removeDirectory($this->dbName);
        }

        $this->log("Using source-root mode (no actual compilation needed)");

        $noopCmd = PHP_OS_FAMILY === 'Windows' ? 'echo CodeQL' : 'echo CodeQL';
        $cmd = sprintf(
            '%s database create %s --language=cpp --command=%s --source-root=. --overwrite',
            escapeshellarg($this->codeqlPath),
            escapeshellarg($this->dbName),
            escapeshellarg($noopCmd)
        );

        $result = $this->exec($cmd);

        if ($result['code'] !== 0) {
            $this->error("Failed to create database:\n" . implode("\n", $result['output']));
        }

        $this->log("✓ Database created successfully");
    }

    public function analyze(string $queryPack = 'cpp-security-and-quality'): void {
        $this->log("Step 2: Running security analysis...");

        if (!is_dir($this->dbName)) {
            $this->error("Database not found. Run createDatabase() first.");
        }

        $sarifFile = 'codeql-results.sarif';
        $cmd = sprintf(
            '%s database analyze %s %s --format=sarif-latest --output=%s',
            escapeshellarg($this->codeqlPath),
            escapeshellarg($this->dbName),
            escapeshellarg($queryPack),
            escapeshellarg($sarifFile)
        );

        $result = $this->exec($cmd);

        if ($result['code'] !== 0) {
            $this->error("Analysis failed:\n" . implode("\n", $result['output']));
        }

        $this->log("✓ SARIF results saved to: {$sarifFile}");

        $csvFile = 'codeql-results.csv';
        $cmd = sprintf(
            '%s database analyze %s %s --format=csv --output=%s',
            escapeshellarg($this->codeqlPath),
            escapeshellarg($this->dbName),
            escapeshellarg($queryPack),
            escapeshellarg($csvFile)
        );

        $this->exec($cmd);
        $this->log("✓ CSV results saved to: {$csvFile}");

        $this->showSummary($sarifFile);
    }

    private function showSummary(string $sarifFile): void {
        if (!file_exists($sarifFile)) {
            return;
        }

        $sarif = json_decode(file_get_contents($sarifFile), true);

        if (!isset($sarif['runs'][0]['results'])) {
            $this->log("\n✓ No security issues found!");
            return;
        }

        $results = $sarif['runs'][0]['results'];
        $total = count($results);

        $byLevel = [];
        foreach ($results as $result) {
            $level = $result['level'] ?? 'note';
            $byLevel[$level] = ($byLevel[$level] ?? 0) + 1;
        }

        echo "\n" . str_repeat('=', 50) . "\n";
        echo "CodeQL Analysis Summary\n";
        echo str_repeat('=', 50) . "\n";
        echo "Total issues found: {$total}\n";
        foreach ($byLevel as $level => $count) {
            $icon = match($level) {
                'error' => '❌',
                'warning' => '⚠️',
                default => 'ℹ️'
            };
            echo "  {$icon} {$level}: {$count}\n";
        }
        echo str_repeat('=', 50) . "\n\n";

        echo "To view detailed results:\n";
        echo "  - Install CodeQL extension in VS Code\n";
        echo "  - Open: {$sarifFile}\n";
        echo "  - Or check: codeql-results.csv\n\n";
    }

    private function removeDirectory(string $dir): void {
        if (PHP_OS_FAMILY === 'Windows') {
            exec('rmdir /s /q ' . escapeshellarg($dir));
        } else {
            exec('rm -rf ' . escapeshellarg($dir));
        }
    }

    public function cleanup(): void {
        $this->log("Cleaning up database...");
        if (is_dir($this->dbName)) {
            $this->removeDirectory($this->dbName);
            $this->log("✓ Database removed");
        }
    }

    public function runFullAnalysis(): void {
        echo "\n";
        echo "╔════════════════════════════════════════════════╗\n";
        echo "║   CodeQL Security Analysis for PHP HTTP Server ║\n";
        echo "╚════════════════════════════════════════════════╝\n";
        echo "\n";

        $this->createDatabase();
        $this->analyze();

        echo "\n✓ Analysis complete!\n\n";
    }
}

$options = getopt('hcq:', ['help', 'cleanup', 'query:']);

if (isset($options['h']) || isset($options['help'])) {
    echo <<<HELP
CodeQL Security Analyzer
Usage: php codeql-analyze.php [options]

Options:
  -h, --help              Show this help message
  -c, --cleanup           Remove CodeQL database and exit
  -q, --query <pack>      Specify query pack (default: cpp-security-and-quality)

Examples:
  php codeql-analyze.php                    # Run full analysis
  php codeql-analyze.php -q cpp-security    # Use specific query pack
  php codeql-analyze.php --cleanup          # Clean up database

Available query packs:
  - cpp-security-and-quality (recommended)
  - cpp-security-extended
  - codeql/cpp-queries

HELP;
    exit(0);
}

$analyzer = new CodeQLAnalyzer();

if (isset($options['c']) || isset($options['cleanup'])) {
    $analyzer->cleanup();
    exit(0);
}

$queryPack = $options['q'] ?? $options['query'] ?? 'cpp-security-and-quality';
$analyzer->runFullAnalysis();
