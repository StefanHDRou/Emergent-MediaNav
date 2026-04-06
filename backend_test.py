#!/usr/bin/env python3
"""
Backend API Testing for MN1 Android Auto Client Documentation Browser
Tests all endpoints specified in the review request.
"""

import requests
import sys
import json
from datetime import datetime

class DocumentationBrowserTester:
    def __init__(self, base_url="https://legacy-autoplay.preview.emergentagent.com"):
        self.base_url = base_url
        self.tests_run = 0
        self.tests_passed = 0
        self.test_results = []

    def run_test(self, name, method, endpoint, expected_status, expected_content_checks=None):
        """Run a single API test with content validation"""
        url = f"{self.base_url}/{endpoint}"
        headers = {'Content-Type': 'application/json'}

        self.tests_run += 1
        print(f"\n🔍 Testing {name}...")
        print(f"   URL: {url}")
        
        try:
            if method == 'GET':
                response = requests.get(url, headers=headers, timeout=10)
            elif method == 'POST':
                response = requests.post(url, headers=headers, timeout=10)

            success = response.status_code == expected_status
            content_valid = True
            content_errors = []

            if success and expected_content_checks:
                try:
                    data = response.json()
                    for check_name, check_func in expected_content_checks.items():
                        if not check_func(data):
                            content_valid = False
                            content_errors.append(check_name)
                except Exception as e:
                    content_valid = False
                    content_errors.append(f"JSON parse error: {str(e)}")

            overall_success = success and content_valid

            if overall_success:
                self.tests_passed += 1
                print(f"✅ Passed - Status: {response.status_code}")
                if expected_content_checks:
                    print(f"   Content validation: ✅ All checks passed")
            else:
                print(f"❌ Failed - Expected {expected_status}, got {response.status_code}")
                if not content_valid:
                    print(f"   Content validation failed: {', '.join(content_errors)}")

            # Store detailed results
            result = {
                "test_name": name,
                "endpoint": endpoint,
                "status_code": response.status_code,
                "expected_status": expected_status,
                "success": overall_success,
                "content_errors": content_errors,
                "response_size": len(response.content) if response.content else 0
            }

            if overall_success and hasattr(response, 'json'):
                try:
                    result["response_data"] = response.json()
                except:
                    result["response_data"] = "Non-JSON response"

            self.test_results.append(result)
            return overall_success, response.json() if overall_success else {}

        except Exception as e:
            print(f"❌ Failed - Error: {str(e)}")
            self.test_results.append({
                "test_name": name,
                "endpoint": endpoint,
                "success": False,
                "error": str(e)
            })
            return False, {}

    def test_health_endpoint(self):
        """Test /api/health endpoint"""
        def check_health_response(data):
            return (
                isinstance(data, dict) and
                data.get("status") == "healthy" and
                "project" in data
            )

        return self.run_test(
            "Health Check",
            "GET",
            "api/health",
            200,
            {"health_format": check_health_response}
        )

    def test_project_tree(self):
        """Test /api/project/tree endpoint"""
        def check_tree_response(data):
            if not isinstance(data, dict):
                return False
            if "files" not in data or "total_files" not in data:
                return False
            files = data.get("files", [])
            total_files = data.get("total_files", 0)
            
            # Should have 42 files as mentioned in requirements
            if total_files < 40:  # Allow some flexibility
                print(f"   Warning: Expected ~42 files, got {total_files}")
            
            # Check file structure
            if not isinstance(files, list) or len(files) == 0:
                return False
                
            # Check first file has required fields
            if files:
                first_file = files[0]
                required_fields = ["path", "name", "size", "lang", "is_doc", "is_source"]
                for field in required_fields:
                    if field not in first_file:
                        print(f"   Missing field in file object: {field}")
                        return False
            
            return True

        success, response = self.run_test(
            "Project File Tree",
            "GET",
            "api/project/tree",
            200,
            {"tree_format": check_tree_response}
        )
        
        if success:
            files = response.get("files", [])
            total_files = response.get("total_files", 0)
            print(f"   📁 Found {total_files} files")
            
            # Count file types
            doc_files = sum(1 for f in files if f.get("is_doc"))
            source_files = sum(1 for f in files if f.get("is_source"))
            print(f"   📄 Documentation files: {doc_files}")
            print(f"   💻 Source files: {source_files}")
            
        return success

    def test_project_stats(self):
        """Test /api/project/stats endpoint"""
        def check_stats_response(data):
            required_fields = ["total_lines", "total_bytes", "file_counts", "total_files"]
            for field in required_fields:
                if field not in data:
                    print(f"   Missing field: {field}")
                    return False
            
            # Check if line count is reasonable (~6966 expected)
            total_lines = data.get("total_lines", 0)
            if total_lines < 5000:  # Allow some flexibility
                print(f"   Warning: Expected ~6966 lines, got {total_lines}")
            
            return True

        success, response = self.run_test(
            "Project Statistics",
            "GET",
            "api/project/stats",
            200,
            {"stats_format": check_stats_response}
        )
        
        if success:
            print(f"   📊 Total lines: {response.get('total_lines', 0):,}")
            print(f"   📁 Total files: {response.get('total_files', 0)}")
            print(f"   💾 Total bytes: {response.get('total_bytes', 0):,}")
            file_counts = response.get('file_counts', {})
            print(f"   📄 File types: {dict(list(file_counts.items())[:5])}")  # Show first 5 types
            
        return success

    def test_file_content(self, file_path, expected_content_type, description):
        """Test /api/project/file endpoint for specific files"""
        def check_file_response(data):
            required_fields = ["path", "content", "lang", "size", "lines"]
            for field in required_fields:
                if field not in data:
                    print(f"   Missing field: {field}")
                    return False
            
            # Check content is not empty
            content = data.get("content", "")
            if not content or len(content.strip()) == 0:
                print(f"   Content is empty")
                return False
                
            # Check language detection
            lang = data.get("lang", "")
            if expected_content_type == "markdown" and lang != "markdown":
                print(f"   Expected markdown, got {lang}")
                return False
            elif expected_content_type == "c" and lang != "c":
                print(f"   Expected c, got {lang}")
                return False
                
            return True

        success, response = self.run_test(
            f"File Content - {description}",
            "GET",
            f"api/project/file?path={file_path}",
            200,
            {"file_format": check_file_response}
        )
        
        if success:
            content = response.get("content", "")
            lines = response.get("lines", 0)
            size = response.get("size", 0)
            lang = response.get("lang", "")
            print(f"   📄 Language: {lang}")
            print(f"   📏 Lines: {lines}")
            print(f"   💾 Size: {size} bytes")
            print(f"   📝 Content preview: {content[:100]}..." if len(content) > 100 else f"   📝 Content: {content}")
            
        return success

def main():
    """Run all backend tests"""
    print("=" * 60)
    print("🧪 MN1 Android Auto Client - Backend API Testing")
    print("=" * 60)
    
    tester = DocumentationBrowserTester()
    
    # Test 1: Health check
    print("\n" + "=" * 40)
    print("TESTING CORE ENDPOINTS")
    print("=" * 40)
    
    tester.test_health_endpoint()
    tester.test_project_tree()
    tester.test_project_stats()
    
    # Test 2: File content endpoints
    print("\n" + "=" * 40)
    print("TESTING FILE CONTENT ENDPOINTS")
    print("=" * 40)
    
    # Test specific files mentioned in requirements
    test_files = [
        ("README.md", "markdown", "Project README"),
        ("src/config.h", "c", "C Header File"),
        ("src/main.c", "c", "Main Source Code"),
        ("docs/03_PERFORMANCE_ANALYSIS.md", "markdown", "Performance Documentation")
    ]
    
    for file_path, content_type, description in test_files:
        tester.test_file_content(file_path, content_type, description)
    
    # Print final results
    print("\n" + "=" * 60)
    print("📊 FINAL TEST RESULTS")
    print("=" * 60)
    print(f"Tests run: {tester.tests_run}")
    print(f"Tests passed: {tester.tests_passed}")
    print(f"Success rate: {(tester.tests_passed/tester.tests_run*100):.1f}%")
    
    # Print failed tests
    failed_tests = [r for r in tester.test_results if not r.get("success", False)]
    if failed_tests:
        print(f"\n❌ Failed tests ({len(failed_tests)}):")
        for test in failed_tests:
            print(f"   - {test['test_name']}: {test.get('error', 'Status/content validation failed')}")
    else:
        print(f"\n✅ All tests passed!")
    
    # Save detailed results
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    results_file = f"/app/backend_test_results_{timestamp}.json"
    
    with open(results_file, 'w') as f:
        json.dump({
            "timestamp": timestamp,
            "summary": {
                "tests_run": tester.tests_run,
                "tests_passed": tester.tests_passed,
                "success_rate": tester.tests_passed/tester.tests_run*100 if tester.tests_run > 0 else 0
            },
            "test_results": tester.test_results
        }, f, indent=2)
    
    print(f"\n📄 Detailed results saved to: {results_file}")
    
    return 0 if tester.tests_passed == tester.tests_run else 1

if __name__ == "__main__":
    sys.exit(main())